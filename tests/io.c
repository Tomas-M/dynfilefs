#define _GNU_SOURCE
#include "../storage.h"
#include <assert.h>
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define IMAGE_SIZE (8*1024*1024)
#define MIB (1024*1024)

static ssize_t write_budget=-1;
static ssize_t log_write_budget=-1;
static size_t write_limit;
static const char *fail_after_marker;
static int fail_sync, fail_log_sync, fail_directory_sync, fail_rename, fail_after_rename, sync_calls, delay_writes, active_writes, overlap, stop_writes;

ssize_t __real_pwrite(int fd, const void *buffer, size_t size, off_t offset);
ssize_t __real_pwrite64(int fd, const void *buffer, size_t size, off_t offset);
ssize_t __wrap_pwrite(int fd, const void *buffer, size_t size, off_t offset);
ssize_t __wrap_pwrite64(int fd, const void *buffer, size_t size, off_t offset);
int __real_fsync(int fd);
int __wrap_fsync(int fd);
int __real_rename(const char *source, const char *target);
int __wrap_rename(const char *source, const char *target);

// Target metadata-log faults without failing the preceding data/index-page flushes.
static int log_descriptor(int fd)
{
    char descriptor[64], path[PATH_MAX];
    assert(snprintf(descriptor, sizeof(descriptor), "/proc/self/fd/%d", fd) < (int)sizeof(descriptor));
    ssize_t length=readlink(descriptor, path, sizeof(path));
    return length >= 4 && !memcmp(path+length-4, ".log", 4);
}

// Faults affect real backing I/O, including short writes inside the production retry loop.
static ssize_t fault_write(int fd, const void *buffer, size_t size, off_t offset,
                          ssize_t (*write_fn)(int, const void *, size_t, off_t))
{
    int log_fault=log_write_budget >= 0 && log_descriptor(fd);
    if (log_fault && !log_write_budget) { errno=ENOSPC; return -1; }
    if (log_fault && size > (size_t)log_write_budget) size=(size_t)log_write_budget;
    if (!write_budget) { errno=ENOSPC; return -1; }
    if (write_budget > 0 && size > (size_t)write_budget) size=(size_t)write_budget;
    if (write_limit && size > write_limit) size=write_limit;
    if (delay_writes)
    {
        if (__atomic_add_fetch(&active_writes, 1, __ATOMIC_RELAXED)>1)
            __atomic_store_n(&overlap, 1, __ATOMIC_RELAXED);
        usleep(10000);
    }
    ssize_t result=write_fn(fd, buffer, size, offset);
    if (delay_writes) __atomic_sub_fetch(&active_writes, 1, __ATOMIC_RELAXED);
    if (write_budget > 0 && result > 0) write_budget-=result;
    if (log_fault && result > 0) log_write_budget-=result;
    return result;
}

// Musl uses the ordinary pwrite symbol with a 64-bit off_t.
ssize_t __wrap_pwrite(int fd, const void *buffer, size_t size, off_t offset)
{
    return fault_write(fd, buffer, size, offset, __real_pwrite);
}

// Glibc redirects large-file builds to pwrite64, including on 32-bit targets.
ssize_t __wrap_pwrite64(int fd, const void *buffer, size_t size, off_t offset)
{
    return fault_write(fd, buffer, size, offset, __real_pwrite64);
}

// Failed synchronization must be reported and remain retryable.
int __wrap_fsync(int fd)
{
    __atomic_add_fetch(&sync_calls, 1, __ATOMIC_RELAXED);
    if (fail_sync || fail_after_rename==2 || (fail_log_sync && log_descriptor(fd))) { errno=EIO; return -1; }
    if (fail_directory_sync || fail_after_marker)
    {
        struct stat info;
        assert(!fstat(fd, &info));
        if (S_ISDIR(info.st_mode))
        {
            if (fail_directory_sync) { errno=EIO; return -1; }
            if (access(fail_after_marker, F_OK) && errno==ENOENT) { errno=EIO; return -1; }
        }
    }
    return __real_fsync(fd);
}

// Fail the final metadata publication after its candidate has been written.
int __wrap_rename(const char *source, const char *target)
{
    if (fail_rename) { errno=EIO; return -1; }
    int result=__real_rename(source, target);
    if (!result && fail_after_rename==1) fail_after_rename=2;
    return result;
}

// Open a small virtual image with deliberately small physical parts.
static struct storage *open_store(const char *path)
{
    struct storage *storage=NULL;
    assert(!storage_open(&storage, path, IMAGE_SIZE, 0, MIB, 0));
    assert(storage_size(storage)==IMAGE_SIZE);
    return storage;
}

// Compare all virtual bytes, including holes and boundaries between leaves and parts.
static void check_bytes(struct storage *storage, const unsigned char *expected)
{
    unsigned char *actual=malloc(IMAGE_SIZE);
    assert(actual && storage_read(storage, actual, IMAGE_SIZE, 0)==IMAGE_SIZE);
    assert(!memcmp(actual, expected, IMAGE_SIZE));
    free(actual);
}

// A successfully closed mount has no transaction, conversion or lock companions.
static void check_clean_files(const char *path)
{
    const char *suffixes[]={".lock", ".log", ".work", ".next", ".migrate", ".rollback"};
    for (size_t i=0; i < sizeof(suffixes)/sizeof(*suffixes); i++)
    {
        char name[PATH_MAX];
        assert(snprintf(name, sizeof(name), "%s%s", path, suffixes[i]) < (int)sizeof(name));
        assert(access(name, F_OK) && errno==ENOENT);
    }
}

// Verify publication failures cannot corrupt either a hole or an immutable original block.
static void check_faults(const char *path)
{
    unsigned char *expected=calloc(1, IMAGE_SIZE), input[3*4096];
    assert(expected);
    memset(input, 'A', sizeof(input));
    struct storage *storage=open_store(path);
    write_budget=0;
    assert(storage_write(storage, input, sizeof(input), 0)==-ENOSPC);
    check_bytes(storage, expected);
    write_budget=4096+17;
    assert(storage_write(storage, input, sizeof(input), 0)==-ENOSPC);
    check_bytes(storage, expected);
    write_budget=-1;
    write_limit=97;
    assert(storage_write(storage, input, sizeof(input), 0)==sizeof(input));
    memcpy(expected, input, sizeof(input));
    write_limit=0;
    fail_sync=1;
    assert(storage_sync(storage)==-EIO);
    fail_sync=0; sync_calls=0;
    assert(!storage_sync(storage) && sync_calls);
    sync_calls=0;
    assert(!storage_sync(storage) && !sync_calls);
    assert(!storage_finish(storage));
    storage_close(storage);

    storage=open_store(path);
    memset(input, 'B', sizeof(input));
    write_budget=37;
    assert(storage_write(storage, input, sizeof(input), 0)==-ENOSPC);
    check_bytes(storage, expected);
    write_budget=-1;
    assert(storage_write(storage, input, sizeof(input), 0)==sizeof(input));
    assert(!storage_sync(storage));
    memcpy(expected, input, sizeof(input));
    for (int fault=0; fault < 2; fault++)
    {
        memset(input, 'C', sizeof(input));
        assert(storage_write(storage, input, sizeof(input), 0)==sizeof(input));
        if (!fault) write_budget=0;
        if (fault==1) fail_sync=1;
        assert(storage_sync(storage)==(!fault ? -ENOSPC : -EIO));
        write_budget=-1; fail_sync=0; fail_rename=0;
        storage_close(storage);
        storage=open_store(path);
        check_bytes(storage, expected);
    }
    assert(storage_write(storage, input, sizeof(input), 0)==sizeof(input));
    assert(!storage_sync(storage));
    memcpy(expected, input, sizeof(input));
    memset(input, 'D', sizeof(input));
    assert(storage_write(storage, input, sizeof(input), 0)==sizeof(input));
    fail_rename=1;
    assert(storage_finish(storage)==-EIO);
    fail_rename=0;
    storage_close(storage);
    storage=open_store(path);
    memcpy(expected, input, sizeof(input));
    check_bytes(storage, expected);
    memset(input, 'E', sizeof(input));
    assert(storage_write(storage, input, sizeof(input), 0)==sizeof(input));
    fail_rename=1;
    assert(storage_finish(storage)==-EIO);
    fail_rename=0;
    assert(!storage_finish(storage));
    memcpy(expected, input, sizeof(input));
    storage_close(storage);
    storage=open_store(path);
    check_bytes(storage, expected);
    memset(input, 'F', sizeof(input));
    assert(storage_write(storage, input, sizeof(input), 0)==sizeof(input));
    fail_after_rename=1;
    assert(storage_finish(storage)==-EIO && fail_after_rename==2);
    fail_after_rename=0;
    memcpy(expected, input, sizeof(input));
    storage_close(storage);
    storage=open_store(path);
    check_bytes(storage, expected);
    assert(!storage_finish(storage));
    storage_close(storage);
    free(expected);
    puts("PASS: ENOSPC, short writes, failed checkpoints, finish publication and sync retry");
}

// Rollback overrides successful checkpoints and later writes after finish or interruption.
static void check_rollback(const char *path)
{
    unsigned char *expected=calloc(1, IMAGE_SIZE), *appended=malloc(2*MIB), block[4096];
    assert(expected && appended);
    memset(appended, 0x54, 2*MIB);
    memset(block, 0x31, sizeof(block));
    struct storage *storage=open_store(path);
    assert(storage_write(storage, block, sizeof(block), 123)==sizeof(block));
    memcpy(expected+123, block, sizeof(block));
    assert(!storage_finish(storage));
    storage_close(storage);
    char part[PATH_MAX], marker[PATH_MAX], lock[PATH_MAX], log[PATH_MAX];
    assert(snprintf(part, sizeof(part), "%s.0", path) < (int)sizeof(part));
    assert(snprintf(marker, sizeof(marker), "%s.rollback", path) < (int)sizeof(marker));
    assert(snprintf(lock, sizeof(lock), "%s.lock", path) < (int)sizeof(lock));
    assert(snprintf(log, sizeof(log), "%s.log", path) < (int)sizeof(log));
    struct stat before, after;
    assert(!stat(part, &before));
    for (int finish=0; finish < 2; finish++)
    {
        storage=open_store(path);
        memset(block, 0x73, sizeof(block));
        assert(storage_write(storage, block, sizeof(block), 0)==sizeof(block));
        assert(storage_write(storage, block, sizeof(block), 7*MIB+71)==sizeof(block));
        assert(storage_write(storage, appended, 2*MIB, 4*MIB)==2*MIB);
        assert(!storage_sync(storage));
        memset(block, 0x85, sizeof(block));
        assert(storage_write(storage, block, sizeof(block), 0)==sizeof(block));
        assert(!storage_sync(storage));
        assert(!storage_get_rollback(storage));
        assert(!storage_set_rollback(storage, 1) && storage_get_rollback(storage));
        assert(!storage_set_rollback(storage, 0) && !storage_get_rollback(storage));
        memset(block, 0x96, sizeof(block));
        assert(storage_write(storage, block, sizeof(block), 0)==sizeof(block));
        assert(!storage_sync(storage));
        assert(!storage_set_rollback(storage, 1));
        assert(storage_write(storage, block, sizeof(block), 3*MIB+17)==sizeof(block));
        assert(!storage_sync(storage));
        assert(storage_write(storage, block, sizeof(block), 6*MIB)==sizeof(block));
        struct stat pending;
        assert(!stat(part, &pending));
        if (finish) assert(!storage_finish(storage));
        storage_close(storage);
        if (finish)
        {
            assert(access(lock, F_OK) && errno==ENOENT);
            assert(!access(marker, F_OK) && !access(log, F_OK));
            assert(!stat(part, &after) && after.st_size==pending.st_size);
        }
        storage=open_store(path);
        assert(!storage_get_rollback(storage));
        check_bytes(storage, expected);
        assert(!stat(part, &after) && after.st_size==before.st_size);
        assert(!storage_finish(storage));
        storage_close(storage);
        check_clean_files(path);
    }
    free(expected); free(appended);
    puts("PASS: rollback remains pending at close and restores data and tails only at next open");
}

// Cancelling a request preserves checkpoints; failed marker flushes remain retryable.
static void check_rollback_cancel(const char *path)
{
    unsigned char *expected=calloc(1, IMAGE_SIZE), block[4096];
    char marker[PATH_MAX];
    assert(expected && snprintf(marker, sizeof(marker), "%s.rollback", path) < (int)sizeof(marker));
    for (int finish=0; finish < 2; finish++)
    {
        struct storage *storage=open_store(path);
        assert(!storage_get_rollback(storage));
        memset(block, 'A'+finish, sizeof(block));
        assert(storage_write(storage, block, sizeof(block), 19)==sizeof(block));
        assert(!storage_sync(storage));
        fail_sync=1;
        assert(storage_set_rollback(storage, 1)==-EIO);
        assert(storage_get_rollback(storage) && !access(marker, F_OK));
        fail_sync=0; sync_calls=0;
        assert(!storage_set_rollback(storage, 1) && sync_calls==2);
        fail_directory_sync=1;
        assert(storage_set_rollback(storage, 0)==-EIO);
        assert(!storage_get_rollback(storage) && access(marker, F_OK) && errno==ENOENT);
        fail_directory_sync=0; sync_calls=0;
        assert(!storage_set_rollback(storage, 0) && sync_calls==1);
        fail_directory_sync=1;
        assert(storage_set_rollback(storage, 1)==-EIO);
        assert(storage_get_rollback(storage) && !access(marker, F_OK));
        fail_directory_sync=0; sync_calls=0;
        assert(!storage_set_rollback(storage, 1) && sync_calls==2);
        memset(block, 'C'+finish, sizeof(block));
        assert(storage_write(storage, block, sizeof(block), 19)==sizeof(block));
        assert(!storage_sync(storage));
        memcpy(expected+19, block, sizeof(block));
        assert(!storage_set_rollback(storage, 0) && !storage_get_rollback(storage));
        assert(access(marker, F_OK) && errno==ENOENT);
        memset(block, 'E'+finish, sizeof(block));
        assert(storage_write(storage, block, sizeof(block), 19)==sizeof(block));
        if (finish)
        {
            assert(!storage_finish(storage));
            memcpy(expected+19, block, sizeof(block));
        }
        storage_close(storage);
        storage=open_store(path);
        assert(!storage_get_rollback(storage));
        check_bytes(storage, expected);
        assert(!storage_finish(storage));
        storage_close(storage);
    }
    free(expected);
    puts("PASS: rollback cancellation, checkpoint recovery and failed marker flush retries");
}

// Continued mounts keep the original baseline while clean exits preserve all current writes.
static void check_norollback(const char *path)
{
    unsigned char *expected=calloc(1, IMAGE_SIZE), block[4096];
    struct storage *storage=NULL;
    assert(expected && !storage_open(&storage, path, IMAGE_SIZE, 0, MIB, 1));
    assert(!storage_get_rollback(storage));
    memset(block, 'A', sizeof(block));
    assert(storage_write(storage, block, sizeof(block), 0)==sizeof(block));
    assert(!storage_finish(storage));
    storage_close(storage);
    storage=open_store(path);
    assert(!storage_set_rollback(storage, 1));
    memset(block, 'B', sizeof(block));
    assert(storage_write(storage, block, sizeof(block), 0)==sizeof(block));
    assert(!storage_finish(storage));
    storage_close(storage);
    // Failed continuation publication must leave the original rollback point usable.
    fail_rename=1;
    storage=NULL;
    assert(storage_open(&storage, path, 0, 0, 0, 1)==-EIO && !storage);
    fail_rename=0;
    storage=open_store(path);
    memset(expected, 'A', sizeof(block));
    check_bytes(storage, expected);
    assert(!storage_get_rollback(storage) && !storage_set_rollback(storage, 1));
    assert(storage_write(storage, block, sizeof(block), 0)==sizeof(block));
    assert(!storage_finish(storage));
    storage_close(storage);
    assert(!storage_open(&storage, path, 0, 0, 0, 1));
    assert(storage_get_rollback(storage));
    memcpy(expected, block, sizeof(block));
    check_bytes(storage, expected);
    memset(block, 'C', sizeof(block));
    assert(storage_write(storage, block, sizeof(block), 0)==sizeof(block));
    assert(storage_write(storage, block, sizeof(block), 6*MIB)==sizeof(block));
    memcpy(expected, block, sizeof(block));
    memcpy(expected+6*MIB, block, sizeof(block));
    assert(!storage_set_rollback(storage, 0) && !storage_set_rollback(storage, 1));
    assert(!storage_finish(storage));
    storage_close(storage);
    assert(!storage_open(&storage, path, 0, 0, 0, 1));
    check_bytes(storage, expected);
    memset(block, 'D', sizeof(block));
    assert(storage_write(storage, block, sizeof(block), 0)==sizeof(block));
    assert(!storage_sync(storage));
    memcpy(expected, block, sizeof(block));
    memset(block, 'E', sizeof(block));
    assert(storage_write(storage, block, sizeof(block), 0)==sizeof(block));
    storage_close(storage);
    assert(!storage_open(&storage, path, 0, 0, 0, 1));
    assert(storage_get_rollback(storage));
    check_bytes(storage, expected);
    assert(!storage_finish(storage));
    storage_close(storage);
    storage=open_store(path);
    memset(expected, 0, IMAGE_SIZE);
    memset(expected, 'A', sizeof(block));
    check_bytes(storage, expected);
    assert(!storage_get_rollback(storage) && !storage_finish(storage));
    storage_close(storage);
    for (int crash=0; crash < 2; crash++)
    {
        storage=open_store(path);
        assert(!storage_set_rollback(storage, 1));
        memset(block, 'B', sizeof(block));
        assert(storage_write(storage, block, sizeof(block), 4096)==sizeof(block));
        memcpy(expected+4096, block, sizeof(block));
        assert(!storage_finish(storage));
        storage_close(storage);
        assert(!storage_open(&storage, path, 0, 0, 0, 1));
        assert(storage_get_rollback(storage) && !storage_set_rollback(storage, 0));
        if (crash)
        {
            memset(block, 'C', sizeof(block));
            assert(storage_write(storage, block, sizeof(block), 0)==sizeof(block));
            assert(!storage_sync(storage));
            memcpy(expected, block, sizeof(block));
            memset(block, 'D', sizeof(block));
            assert(storage_write(storage, block, sizeof(block), 0)==sizeof(block));
        }
        else assert(!storage_finish(storage));
        storage_close(storage);
        storage=open_store(path);
        check_bytes(storage, expected);
        assert(!storage_get_rollback(storage) && !storage_finish(storage));
        storage_close(storage);
        check_clean_files(path);
    }
    free(expected);
    puts("PASS: continued rollback preserves baseline through clean writes, crashes, cancellation and publication failure");
}

// Commit cleanup fails at close; deferred rollback cleanup can fail only at the next open.
static void check_cleanup(const char *path, int mode)
{
    unsigned char *expected=calloc(1, IMAGE_SIZE), block[4096];
    char marker[PATH_MAX], lock[PATH_MAX], log[PATH_MAX];
    assert(expected && snprintf(marker, sizeof(marker), "%s.rollback", path) < (int)sizeof(marker));
    assert(snprintf(lock, sizeof(lock), "%s.lock", path) < (int)sizeof(lock));
    assert(snprintf(log, sizeof(log), "%s.log", path) < (int)sizeof(log));
    struct storage *storage=open_store(path);
    memset(block, 'A', sizeof(block));
    assert(storage_write(storage, block, sizeof(block), 0)==sizeof(block));
    assert(!storage_finish(storage));
    storage_close(storage);
    check_clean_files(path);
    storage=open_store(path);
    memset(block, 'B', sizeof(block));
    assert(storage_write(storage, block, sizeof(block), 0)==sizeof(block));
    assert(!storage_sync(storage));
    if (mode < 2)
    {
        fail_directory_sync=1;
        assert(storage_finish(storage)==-EIO);
        fail_directory_sync=0;
        assert(!access(log, F_OK));
        if (mode) assert(!storage_finish(storage));
        storage_close(storage);
        if (mode) check_clean_files(path);
        else assert(!access(lock, F_OK));
    }
    else
    {
        assert(!storage_set_rollback(storage, 1));
        assert(!storage_finish(storage));
        storage_close(storage);
        assert(!access(marker, F_OK) && !access(log, F_OK));
        assert(access(lock, F_OK) && errno==ENOENT);
        if (mode==3) fail_after_marker=marker;
        else fail_directory_sync=1;
        storage=NULL;
        assert(storage_open(&storage, path, 0, 0, 0, 0)==-EIO && !storage);
        fail_directory_sync=0; fail_after_marker=NULL;
        assert(!access(lock, F_OK));
        if (mode==2) assert(!access(marker, F_OK));
        else
        {
            assert(access(marker, F_OK) && errno==ENOENT);
            assert(access(log, F_OK) && errno==ENOENT);
        }
    }
    storage=open_store(path);
    memset(expected, mode >= 2 ? 'A' : 'B', sizeof(block));
    check_bytes(storage, expected);
    assert(!storage_finish(storage));
    storage_close(storage);
    check_clean_files(path);
    free(expected);
}

// The most recent complete fsync survives close while following unsynchronized writes do not.
static void check_checkpoints(const char *path)
{
    unsigned char *expected=calloc(1, IMAGE_SIZE), block[4096];
    assert(expected);
    struct storage *storage=open_store(path);
    memset(block, 'A', sizeof(block));
    assert(storage_write(storage, block, sizeof(block), 17)==sizeof(block));
    memcpy(expected+17, block, sizeof(block));
    assert(!storage_finish(storage));
    storage_close(storage);
    storage=open_store(path);
    for (int value='B'; value <= 'C'; value++)
    {
        memset(block, value, sizeof(block));
        assert(storage_write(storage, block, sizeof(block), 17)==sizeof(block));
        assert(storage_write(storage, block, sizeof(block), 6*MIB+19)==sizeof(block));
        memcpy(expected+17, block, sizeof(block));
        memcpy(expected+6*MIB+19, block, sizeof(block));
        assert(!storage_sync(storage));
    }
    memset(block, 'D', sizeof(block));
    assert(storage_write(storage, block, sizeof(block), 17)==sizeof(block));
    assert(storage_write(storage, block, sizeof(block), 4*MIB)==sizeof(block));
    storage_close(storage);
    storage=open_store(path);
    check_bytes(storage, expected);
    assert(!storage_finish(storage));
    storage_close(storage);
    free(expected);
    puts("PASS: interrupted mount recovers the last successful checkpoint");
}

// Missing or reset logs occupy zero bytes; every completed record is page aligned.
static off_t log_size(const char *path)
{
    struct stat info;
    if (!stat(path, &info)) return info.st_size;
    assert(errno==ENOENT);
    return 0;
}

// An incomplete append keeps the previous checkpoint; uncertain complete appends stay pinned.
static void check_journal_faults(const char *path)
{
    unsigned char *expected=calloc(1, IMAGE_SIZE), block[4096];
    char log[PATH_MAX], work[PATH_MAX];
    assert(expected && snprintf(log, sizeof(log), "%s.log", path) < (int)sizeof(log));
    assert(snprintf(work, sizeof(work), "%s.work", path) < (int)sizeof(work));
    for (int fault=0; fault < 4; fault++)
    {
        struct storage *storage=open_store(path);
        memset(block, 'B', sizeof(block));
        assert(storage_write(storage, block, sizeof(block), 0)==sizeof(block));
        assert(!storage_sync(storage) && log_size(log)==4096);
        memcpy(expected, block, sizeof(block));
        memset(block, 'C', sizeof(block));
        assert(storage_write(storage, block, sizeof(block), 0)==sizeof(block));
        if (fault==2) fail_log_sync=1;
        else log_write_budget=fault ? 17 : 0;
        assert(storage_sync(storage)==(fault==2 ? -EIO : -ENOSPC));
        log_write_budget=-1; fail_log_sync=0;
        if (fault==2) memcpy(expected, block, sizeof(block));
        memset(block, 'D', sizeof(block));
        assert(storage_write(storage, block, sizeof(block), 0)==sizeof(block));
        if (fault==3)
        {
            assert(!storage_sync(storage) && !log_size(log) && !access(work, F_OK));
            memcpy(expected, block, sizeof(block));
            memset(block, 'E', sizeof(block));
            assert(storage_write(storage, block, sizeof(block), 0)==sizeof(block));
        }
        storage_close(storage);
        storage=open_store(path);
        check_bytes(storage, expected);
        assert(!storage_finish(storage));
        storage_close(storage);
    }
    free(expected);
    puts("PASS: torn journal appends, uncertain fsync protection and full-checkpoint retry");
}

// Recovery accepts only a complete valid prefix and refuses corruption before a valid record.
static void check_journal_tail(const char *path, int mode)
{
    unsigned char *expected=calloc(1, IMAGE_SIZE), block[4096];
    char log[PATH_MAX];
    assert(expected && snprintf(log, sizeof(log), "%s.log", path) < (int)sizeof(log));
    struct storage *storage=open_store(path);
    for (int value='B'; value <= 'C'; value++)
    {
        memset(block, value, sizeof(block));
        assert(storage_write(storage, block, sizeof(block), 0)==sizeof(block));
        assert(!storage_sync(storage));
    }
    storage_close(storage);
    assert(log_size(log)==8192);
    int fd=open(log, O_RDWR);
    assert(fd >= 0);
    if (!mode) assert(!ftruncate(fd, 4096+17));
    else if (mode==3) assert(pwrite(fd, "incomplete suffix", 17, 8192)==17);
    else
    {
        off_t offset=mode==1 ? 4096+16 : 16;
        unsigned char checksum;
        assert(pread(fd, &checksum, 1, offset)==1);
        checksum^=1;
        assert(pwrite(fd, &checksum, 1, offset)==1);
    }
    close(fd);
    storage=NULL;
    if (mode==2)
    {
        assert(storage_open(&storage, path, 0, 0, 0, 0)==-EIO && !storage);
        assert(log_size(log)==8192);
    }
    else
    {
        storage=open_store(path);
        memset(expected, mode==3 ? 'C' : 'B', sizeof(block));
        check_bytes(storage, expected);
        assert(!storage_finish(storage));
        storage_close(storage);
    }
    free(expected);
}

// Exercise the real 32 MiB cap, publication faults, and replay after compaction.
static void check_journal_limit(const char *path)
{
    unsigned char *expected=calloc(1, IMAGE_SIZE), block[4096];
    char log[PATH_MAX], work[PATH_MAX];
    assert(expected && snprintf(log, sizeof(log), "%s.log", path) < (int)sizeof(log));
    assert(snprintf(work, sizeof(work), "%s.work", path) < (int)sizeof(work));
    struct storage *storage=open_store(path);
    for (int round=0; round < 2; round++)
    {
        memset(block, 'B', sizeof(block));
        for (int checkpoint=0; checkpoint < 8192; checkpoint++)
        {
            assert(storage_write(storage, block, sizeof(block), 0)==sizeof(block));
            assert(!storage_sync(storage));
        }
        assert(log_size(log)==32*MIB);
        sync_calls=0;
        assert(!storage_sync(storage) && !sync_calls && log_size(log)==32*MIB);
        memset(block, 'C'+round, sizeof(block));
        assert(storage_write(storage, block, sizeof(block), 0)==sizeof(block));
        if (!round)
        {
            fail_rename=1;
            assert(storage_sync(storage)==-EIO && log_size(log)==32*MIB);
            fail_rename=0; fail_after_rename=1;
            assert(storage_sync(storage)==-EIO && fail_after_rename==2);
            fail_after_rename=0;
        }
        else assert(!storage_sync(storage) && !log_size(log));
        assert(!access(work, F_OK));
        size_t stale_size=(size_t)log_size(work);
        unsigned char *stale=malloc(stale_size);
        int fd=open(work, O_RDONLY);
        assert(stale && fd >= 0 && pread(fd, stale, stale_size, 0)==(ssize_t)stale_size);
        close(fd);
        memcpy(expected, block, sizeof(block));
        memset(block, 'Z', sizeof(block));
        assert(storage_write(storage, block, sizeof(block), 0)==sizeof(block));
        if (round)
        {
            assert(!storage_sync(storage) && log_size(log)==4096);
            memcpy(expected, block, sizeof(block));
        }
        storage_close(storage);
        storage=open_store(path);
        check_bytes(storage, expected);
        assert(!log_size(log) && access(work, F_OK) && errno==ENOENT);
        // Simulate interruption after consolidating main but before removing an older .work.
        if (round)
        {
            storage_close(storage);
            fd=open(work, O_WRONLY|O_CREAT|O_TRUNC, 0600);
            assert(fd >= 0 && pwrite(fd, stale, stale_size, 0)==(ssize_t)stale_size);
            close(fd);
            storage=open_store(path);
            check_bytes(storage, expected);
            assert(access(work, F_OK) && errno==ENOENT);
        }
        free(stale);
    }
    assert(!storage_finish(storage));
    storage_close(storage);
    free(expected);
    puts("PASS: 8192 records fit 32 MiB, compaction faults and post-compaction replay");
}

struct writer_args
{
    struct storage *storage;
    pthread_barrier_t *barrier;
    off_t offset;
    size_t size;
    int value, repeat;
};

// Fixed disjoint byte ranges give concurrent partial writes an unambiguous oracle.
static void *writer(void *data)
{
    struct writer_args *args=data;
    unsigned char buffer[4096];
    memset(buffer, args->value, sizeof(buffer));
    pthread_barrier_wait(args->barrier);
    do assert(storage_write(args->storage, buffer, args->size, args->offset)==(int)args->size);
    while (args->repeat && !__atomic_load_n(&stop_writes, __ATOMIC_RELAXED));
    return NULL;
}

// Synchronization must progress without serializing independent leaf writes.
static void check_concurrency(const char *path)
{
    unsigned char *expected=calloc(1, IMAGE_SIZE);
    assert(expected);
    struct storage *storage=open_store(path);
    pthread_t threads[8];
    pthread_barrier_t barrier;
    struct writer_args args[8];
    pthread_barrier_init(&barrier, NULL, 8);
    for (int i=0; i < 8; i++)
    {
        args[i]=(struct writer_args){storage, &barrier, MIB+i*512, 512, i+1, 0};
        memset(expected+args[i].offset, i+1, 512);
        assert(!pthread_create(&threads[i], NULL, writer, &args[i]));
    }
    for (int i=0; i < 8; i++) assert(!pthread_join(threads[i], NULL));
    pthread_barrier_destroy(&barrier);
    pthread_barrier_init(&barrier, NULL, 2);
    delay_writes=1;
    for (int i=0; i < 2; i++)
    {
        args[i]=(struct writer_args){storage, &barrier, (2+i*2)*MIB, 4096, 20+i, 0};
        memset(expected+args[i].offset, args[i].value, 4096);
        assert(!pthread_create(&threads[i], NULL, writer, &args[i]));
    }
    for (int i=0; i < 2; i++) assert(!pthread_join(threads[i], NULL));
    assert(overlap);
    delay_writes=0;
    pthread_barrier_destroy(&barrier);

    pthread_barrier_init(&barrier, NULL, 9);
    for (int i=0; i < 8; i++)
    {
        args[i]=(struct writer_args){storage, &barrier, MIB+i*512, 512, i+1, 1};
        assert(!pthread_create(&threads[i], NULL, writer, &args[i]));
    }
    pthread_barrier_wait(&barrier);
    usleep(20000);
    alarm(10);
    for (int i=0; i < 16; i++) assert(!storage_sync(storage));
    alarm(0);
    __atomic_store_n(&stop_writes, 1, __ATOMIC_RELAXED);
    for (int i=0; i < 8; i++) assert(!pthread_join(threads[i], NULL));
    pthread_barrier_destroy(&barrier);
    check_bytes(storage, expected);
    assert(!storage_finish(storage));
    storage_close(storage);
    storage=open_store(path);
    check_bytes(storage, expected);
    assert(!storage_finish(storage));
    storage_close(storage);
    free(expected);
    puts("PASS: partial-block races, parallel leaves and fsync progress");
}

// Lazy files, sparse zeros, bounds and descriptor reads remain part of the public contract.
static void check_io(const char *path)
{
    struct storage *storage=open_store(path);
    unsigned char zero[4096]={0}, input[3*4096], output[3*4096];
    char part[PATH_MAX];
    assert(snprintf(part, sizeof(part), "%s.0", path) < (int)sizeof(part));
    struct storage *other=NULL;
    assert(storage_open(&other, path, IMAGE_SIZE, 0, MIB, 0)==-EWOULDBLOCK && !other);
    assert(access(part, F_OK) && errno==ENOENT);
    assert(storage_write(storage, zero, sizeof(zero), 7*MIB)==sizeof(zero));
    assert(!storage_sync(storage) && access(part, F_OK) && errno==ENOENT);
    assert(storage_read(storage, output, 1, -1)==-EINVAL);
    assert(storage_write(storage, input, 1, -1)==-EINVAL);
    assert(storage_write(storage, input, 1, IMAGE_SIZE)==-ENOSPC);
    assert(!storage_read(storage, output, 1, IMAGE_SIZE));
    for (size_t i=0; i < sizeof(input); i++) input[i]=(unsigned char)(i*17+1);
    assert(storage_write(storage, input, sizeof(input), 0)==sizeof(input));
    int fd;
    off_t offset;
    assert(!storage_read_extent(storage, 0, sizeof(input), &fd, &offset));
    assert(!storage_read_extent(storage, 2*MIB-512, 4096, &fd, &offset));
    assert(!storage_read_extent(storage, IMAGE_SIZE, 4096, &fd, &offset));
    assert(storage_read_extent(storage, -1, 4096, &fd, &offset)==-EINVAL);
    assert(!storage_finish(storage));
    storage_close(storage);
    storage=open_store(path);
    assert(storage_read(storage, output, sizeof(output), 0)==sizeof(output));
    assert(!memcmp(input, output, sizeof(input)));
    assert(storage_read_extent(storage, 0, sizeof(input), &fd, &offset)==1);
    assert(pread(fd, output, sizeof(output), offset)==sizeof(output));
    assert(!memcmp(input, output, sizeof(input)));
    unsigned char changed[sizeof(input)];
    memset(changed, 0x63, sizeof(changed));
    // A returned baseline extent remains valid while later checkpoints recycle other slots.
    for (int checkpoint=0; checkpoint < 3; checkpoint++)
    {
        assert(storage_write(storage, changed, sizeof(changed), 0)==sizeof(changed));
        assert(!storage_sync(storage));
        int current_fd;
        off_t current_offset;
        assert(!storage_read_extent(storage, 0, sizeof(changed), &current_fd, &current_offset));
        assert(pread(fd, output, sizeof(output), offset)==sizeof(output));
        assert(!memcmp(input, output, sizeof(input)));
    }
    assert(!storage_finish(storage));
    storage_close(storage);
    puts("PASS: lazy files, sparse zeros, bounds and descriptor-backed reads");
}

// A large physical window must not narrow to a zero-length allocation on i686.
static void check_large_part(const char *path)
{
    struct storage *storage=NULL;
    uint64_t limit=UINT64_C(16)*1024*1024*1024*1024;
    unsigned char input[4096], output[4096];
    memset(input, 0x9a, sizeof(input));
    assert(!storage_open(&storage, path, MIB, 0, limit, 0));
    alarm(10);
    assert(storage_write(storage, input, sizeof(input), 0)==sizeof(input));
    alarm(0);
    assert(!storage_finish(storage));
    storage_close(storage);
    assert(!storage_open(&storage, path, 0, 0, 0, 0));
    assert(storage_size(storage)==MIB);
    assert(storage_read(storage, output, sizeof(output), 0)==sizeof(output));
    assert(!memcmp(input, output, sizeof(input)));
    assert(!storage_finish(storage));
    storage_close(storage);
    puts("PASS: large physical windows with small allocations");
}

// Malformed metadata must fail before startup can trim or recycle backing parts.
static void check_metadata(const char *path)
{
    struct storage *storage=open_store(path);
    unsigned char input[4096];
    memset(input, 0x2b, sizeof(input));
    assert(storage_write(storage, input, sizeof(input), 0)==sizeof(input));
    assert(!storage_finish(storage));
    storage_close(storage);
    int fd=open(path, O_RDWR);
    struct stat st;
    assert(fd >= 0 && !fstat(fd, &st));
    size_t length=(size_t)st.st_size;
    unsigned char *original=malloc(length), *damaged=malloc(length);
    assert(original && damaged && pread(fd, original, length, 0)==(ssize_t)length);
    memcpy(damaged, original, length);
    damaged[4096]^=1;
    assert(pwrite(fd, damaged, length, 0)==(ssize_t)length);
    storage=NULL;
    assert(storage_open(&storage, path, 0, 0, 0, 0)==-EIO && !storage);
    assert(pwrite(fd, original, length, 0)==(ssize_t)length);
    assert(!ftruncate(fd, (off_t)length-1));
    assert(storage_open(&storage, path, 0, 0, 0, 0)==-EIO && !storage);
    assert(pwrite(fd, original, length, 0)==(ssize_t)length);
    uint64_t geometry[]={htole64(4096), htole64((uint64_t)DYN_MAX_PARTS+1)};
    assert(pwrite(fd, geometry, sizeof(geometry), 24)==sizeof(geometry));
    assert(storage_open(&storage, path, 0, 0, 0, 0)==-EINVAL && !storage);
    assert(pwrite(fd, original, length, 0)==(ssize_t)length);
    close(fd);
    free(original); free(damaged);
    storage=open_store(path);
    unsigned char output[4096];
    assert(storage_read(storage, output, sizeof(output), 0)==sizeof(output));
    assert(!memcmp(input, output, sizeof(input)));
    assert(!storage_finish(storage));
    storage_close(storage);
    puts("PASS: checksum, metadata length and geometry validation");
}

// An incomplete index page must fail its lazy read rather than supply invented zero entries.
static void check_short_leaf(const char *path)
{
    struct storage *storage=open_store(path);
    unsigned char input[4096], output[4096];
    memset(input, 1, sizeof(input));
    write_budget=0;
    assert(storage_write(storage, input, sizeof(input), 0)==-ENOSPC);
    write_budget=-1;
    assert(!storage_finish(storage));
    storage_close(storage);
    char part[PATH_MAX];
    assert(snprintf(part, sizeof(part), "%s.0", path) < (int)sizeof(part));
    int fd=open(part, O_WRONLY);
    assert(fd >= 0 && !ftruncate(fd, 4095));
    close(fd);
    storage=open_store(path);
    assert(storage_read(storage, output, sizeof(output), 0)==-EIO);
    storage_close(storage);
    puts("PASS: truncated lazy index page reports read failure");
}

// Keep every test image in a uniquely owned directory supplied by the test runner.
int main(int argc, char **argv)
{
    if (argc!=2) { fprintf(stderr, "usage: %s temporary-directory\n", argv[0]); return 2; }
    char directory[PATH_MAX], path[PATH_MAX];
    assert(snprintf(directory, sizeof(directory), "%s/io-XXXXXX", argv[1]) < (int)sizeof(directory));
    assert(mkdtemp(directory));
    assert(snprintf(path, sizeof(path), "%s/io", directory) < (int)sizeof(path)); check_io(path);
    assert(snprintf(path, sizeof(path), "%s/faults", directory) < (int)sizeof(path)); check_faults(path);
    assert(snprintf(path, sizeof(path), "%s/checkpoints", directory) < (int)sizeof(path)); check_checkpoints(path);
    assert(snprintf(path, sizeof(path), "%s/journal-faults", directory) < (int)sizeof(path)); check_journal_faults(path);
    for (int mode=0; mode < 4; mode++)
    {
        assert(snprintf(path, sizeof(path), "%s/journal-tail-%d", directory, mode) < (int)sizeof(path));
        check_journal_tail(path, mode);
    }
    puts("PASS: journal truncated/corrupt suffix recovery and middle-corruption rejection");
    assert(snprintf(path, sizeof(path), "%s/journal-limit", directory) < (int)sizeof(path)); check_journal_limit(path);
    assert(snprintf(path, sizeof(path), "%s/rollback", directory) < (int)sizeof(path)); check_rollback(path);
    assert(snprintf(path, sizeof(path), "%s/cancel", directory) < (int)sizeof(path)); check_rollback_cancel(path);
    assert(snprintf(path, sizeof(path), "%s/continue", directory) < (int)sizeof(path)); check_norollback(path);
    for (int mode=0; mode < 4; mode++)
    {
        assert(snprintf(path, sizeof(path), "%s/cleanup-%d", directory, mode) < (int)sizeof(path));
        check_cleanup(path, mode);
    }
    puts("PASS: normal shutdown cleanup and failed deferred-rollback recovery remain retryable");
    assert(snprintf(path, sizeof(path), "%s/concurrent", directory) < (int)sizeof(path)); check_concurrency(path);
    assert(snprintf(path, sizeof(path), "%s/large", directory) < (int)sizeof(path)); check_large_part(path);
    assert(snprintf(path, sizeof(path), "%s/metadata", directory) < (int)sizeof(path)); check_metadata(path);
    assert(snprintf(path, sizeof(path), "%s/short-leaf", directory) < (int)sizeof(path)); check_short_leaf(path);
    return 0;
}
