#define _GNU_SOURCE
#include "../storage.h"
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define REGION_SIZE (UINT64_C(2)*1024*1024)
#define MODEL_SIZE 8192
#define REGION_COUNT (DYN_CACHE_PAGES+257)
#define STRIPE_PAGES (DYN_CACHE_PAGES/256)

static ssize_t write_budget=-1;
static size_t write_limit;
static size_t write_calls, read_calls;
static int sync_calls, fail_next_read;

ssize_t __real_pwrite(int fd, const void *buffer, size_t size, off_t offset);
ssize_t __real_pwrite64(int fd, const void *buffer, size_t size, off_t offset);
ssize_t __wrap_pwrite(int fd, const void *buffer, size_t size, off_t offset);
ssize_t __wrap_pwrite64(int fd, const void *buffer, size_t size, off_t offset);
ssize_t __real_pread(int fd, void *buffer, size_t size, off_t offset);
ssize_t __real_pread64(int fd, void *buffer, size_t size, off_t offset);
ssize_t __wrap_pread(int fd, void *buffer, size_t size, off_t offset);
ssize_t __wrap_pread64(int fd, void *buffer, size_t size, off_t offset);
int __real_fsync(int fd);
int __wrap_fsync(int fd);

// Inject real short writes and failures without replacing the storage retry logic.
static ssize_t fault_write(int fd, const void *buffer, size_t size, off_t offset,
                           ssize_t (*write_fn)(int, const void *, size_t, off_t))
{
    __atomic_add_fetch(&write_calls, 1, __ATOMIC_RELAXED);
    if (!write_budget) { errno=ENOSPC; return -1; }
    if (write_budget > 0 && size > (size_t)write_budget) size=(size_t)write_budget;
    if (write_limit && size > write_limit) size=write_limit;
    ssize_t result=write_fn(fd, buffer, size, offset);
    if (write_budget > 0 && result > 0) write_budget-=result;
    return result;
}

// Musl uses the ordinary symbol with a 64-bit off_t.
ssize_t __wrap_pwrite(int fd, const void *buffer, size_t size, off_t offset)
{
    return fault_write(fd, buffer, size, offset, __real_pwrite);
}

// Glibc redirects large-file builds to pwrite64.
ssize_t __wrap_pwrite64(int fd, const void *buffer, size_t size, off_t offset)
{
    return fault_write(fd, buffer, size, offset, __real_pwrite64);
}

// Count cache misses separately from writes and fail one incoming page read.
static ssize_t fault_read(int fd, void *buffer, size_t size, off_t offset,
                          ssize_t (*read_fn)(int, void *, size_t, off_t))
{
    __atomic_add_fetch(&read_calls, 1, __ATOMIC_RELAXED);
    if (fail_next_read) { fail_next_read=0; errno=EIO; return -1; }
    return read_fn(fd, buffer, size, offset);
}

// Cover native musl reads.
ssize_t __wrap_pread(int fd, void *buffer, size_t size, off_t offset)
{
    return fault_read(fd, buffer, size, offset, __real_pread);
}

// Cover glibc large-file reads.
ssize_t __wrap_pread64(int fd, void *buffer, size_t size, off_t offset)
{
    return fault_read(fd, buffer, size, offset, __real_pread64);
}

// Eviction may write private pages but must never create an implicit checkpoint.
int __wrap_fsync(int fd)
{
    __atomic_add_fetch(&sync_calls, 1, __ATOMIC_RELAXED);
    return __real_fsync(fd);
}

// Use more distinct index pages than the configured cache can retain.
static struct storage *open_store(const char *path, const char *restore)
{
    struct storage *storage=NULL;
    assert(!storage_open(&storage, path, (REGION_COUNT+1)*REGION_SIZE, 0, 4*1024*1024, restore, NULL, 0));
    return storage;
}

// Distinguish regions and write generations without large static fixtures.
static void fill_pattern(unsigned char *bytes, size_t size, size_t seed)
{
    for (size_t i=0; i < size; i++) bytes[i]=(unsigned char)(1+(seed*31+i*17)%251);
}

// Compare stored data and zero gaps while making every index page compete for cache.
static void check_regions(struct storage *storage, const unsigned char *expected, int reverse)
{
    unsigned char actual[MODEL_SIZE];
    for (size_t step=0; step < REGION_COUNT; step++)
    {
        size_t region=reverse ? REGION_COUNT-1-step : step;
        assert(storage_read(storage, actual, sizeof(actual), region*REGION_SIZE)==sizeof(actual));
        assert(!memcmp(actual, expected+region*MODEL_SIZE, sizeof(actual)));
    }
}

// Change partial blocks, including an initially absent entry and a 4 KiB boundary.
static void change_regions(struct storage *storage, unsigned char *expected, size_t offset, size_t generation)
{
    unsigned char patch[89];
    for (size_t region=0; region < REGION_COUNT; region++)
    {
        fill_pattern(patch, sizeof(patch), generation*REGION_COUNT+region);
        assert(storage_write(storage, patch, sizeof(patch), region*REGION_SIZE+offset)==sizeof(patch));
        memcpy(expected+region*MODEL_SIZE+offset, patch, sizeof(patch));
    }
}

// Dirty eviction must preserve both the mount-start root and the last explicit checkpoint.
static void check_pressure(const char *path)
{
    size_t bytes=(size_t)REGION_COUNT*MODEL_SIZE;
    unsigned char *expected=calloc(1, bytes), *durable=malloc(bytes);
    assert(expected && durable);
    struct storage *storage=open_store(path, NULL);
    sync_calls=0;
    for (size_t region=0; region < REGION_COUNT; region++)
    {
        unsigned char *block=expected+region*MODEL_SIZE;
        fill_pattern(block, 4096, region);
        assert(storage_write(storage, block, 4096, region*REGION_SIZE)==4096);
    }
    size_t before=write_calls;
    check_regions(storage, expected, 0);
    assert(write_calls > before && !sync_calls);
    // Reloaded pages were already queued as dirty; their newly added entries must spill too.
    change_regions(storage, expected, 4096+137, 1);
    check_regions(storage, expected, 1);
    assert(!sync_calls);
    assert(!storage_sync(storage));
    memcpy(durable, expected, bytes);

    sync_calls=0;
    change_regions(storage, expected, 4096-31, 2);
    check_regions(storage, expected, 0);
    assert(!sync_calls);
    storage_close(storage);
    storage=open_store(path, NULL);
    check_regions(storage, durable, 1);
    assert(!storage_finish(storage));
    storage_close(storage);

    storage=open_store(path, NULL);
    assert(!storage_snapshot_create(storage, "durable"));
    memcpy(expected, durable, bytes);
    change_regions(storage, expected, 4096-31, 3);
    assert(!storage_sync(storage) && !storage_finish(storage));
    storage_close(storage);
    storage=open_store(path, NULL);
    check_regions(storage, expected, 0);
    change_regions(storage, expected, MODEL_SIZE-99, 4);
    assert(!storage_sync(storage));
    storage_close(storage);
    storage=open_store(path, NULL);
    check_regions(storage, expected, 1);
    assert(!storage_finish(storage));
    storage_close(storage);
    storage=open_store(path, "durable");
    check_regions(storage, durable, 0);
    assert(!storage_finish(storage));
    storage_close(storage);
    free(expected); free(durable);
    puts("PASS: cache pressure, dirty reloads, partial writes, checkpoint recovery and named restores");
}

// These region numbers collide under the documented stripe hash for both test cache sizes.
static off_t collision_offset(size_t index)
{
    return (off_t)(index*257*REGION_SIZE+7*4096);
}

// Failed or partially written victims must remain readable even while backing writes fail.
static void check_eviction_fault(const char *path, ssize_t budget)
{
    struct storage *storage=open_store(path, NULL);
    unsigned char input[4096], output[4096];
    for (size_t i=0; i < STRIPE_PAGES; i++)
    {
        fill_pattern(input, sizeof(input), i);
        assert(storage_write(storage, input, sizeof(input), collision_offset(i))==sizeof(input));
    }
    sync_calls=0; write_budget=budget;
    fill_pattern(input, sizeof(input), STRIPE_PAGES);
    assert(storage_write(storage, input, sizeof(input), collision_offset(STRIPE_PAGES))==-ENOSPC);
    assert(!write_budget && !sync_calls);
    for (size_t i=0; i < STRIPE_PAGES; i++)
    {
        fill_pattern(input, sizeof(input), i);
        assert(storage_read(storage, output, sizeof(output), collision_offset(i))==sizeof(output));
        assert(!memcmp(input, output, sizeof(input)));
    }
    write_budget=-1; write_limit=97;
    fill_pattern(input, sizeof(input), STRIPE_PAGES);
    assert(storage_write(storage, input, sizeof(input), collision_offset(STRIPE_PAGES))==sizeof(input));
    unsigned char patch[37];
    memset(patch, 0xe3, sizeof(patch));
    assert(storage_write(storage, patch, sizeof(patch), collision_offset(0)+2*4096+19)==sizeof(patch));
    for (size_t i=0; i <= STRIPE_PAGES; i++)
    {
        fill_pattern(input, sizeof(input), i);
        assert(storage_read(storage, output, sizeof(output), collision_offset(i))==sizeof(output));
        assert(!memcmp(input, output, sizeof(input)));
    }
    assert(storage_read(storage, output, sizeof(patch), collision_offset(0)+2*4096+19)==sizeof(patch));
    assert(!memcmp(output, patch, sizeof(patch)) && !sync_calls);
    assert(!storage_finish(storage));
    write_limit=0;
    storage_close(storage);
    storage=open_store(path, NULL);
    for (size_t i=0; i <= STRIPE_PAGES; i++)
    {
        fill_pattern(input, sizeof(input), i);
        assert(storage_read(storage, output, sizeof(output), collision_offset(i))==sizeof(output));
        assert(!memcmp(input, output, sizeof(input)));
    }
    assert(storage_read(storage, output, sizeof(patch), collision_offset(0)+2*4096+19)==sizeof(patch));
    assert(!memcmp(output, patch, sizeof(patch)) && !storage_finish(storage));
    storage_close(storage);
}

// A failed incoming leaf read must leave every previous resident usable without a reload.
static void check_read_fault(const char *path)
{
    struct storage *storage=open_store(path, NULL);
    unsigned char input[4096], value;
    for (size_t i=0; i <= STRIPE_PAGES; i++)
    {
        fill_pattern(input, sizeof(input), i);
        assert(storage_write(storage, input, sizeof(input), collision_offset(i))==sizeof(input));
    }
    assert(!storage_finish(storage));
    storage_close(storage);
    storage=open_store(path, NULL);
    for (size_t i=0; i < STRIPE_PAGES; i++)
        assert(storage_read(storage, &value, 1, collision_offset(i))==1);
    value=0x5e;
    assert(storage_write(storage, &value, 1, collision_offset(0)+4096)==1);
    for (size_t i=1; i < STRIPE_PAGES; i++)
        assert(storage_read(storage, &value, 1, collision_offset(i))==1);
    sync_calls=0; fail_next_read=1;
    assert(storage_read(storage, &value, 1, collision_offset(STRIPE_PAGES))==-EIO);
    assert(!fail_next_read);
    read_calls=0;
    for (size_t i=0; i < STRIPE_PAGES; i++)
    {
        fill_pattern(input, 1, i);
        assert(storage_read(storage, &value, 1, collision_offset(i))==1 && value==input[0]);
    }
    assert(read_calls==STRIPE_PAGES);
    assert(storage_read(storage, &value, 1, collision_offset(0)+4096)==1 && value==0x5e);
    assert(storage_read(storage, &value, 1, collision_offset(STRIPE_PAGES))==1);
    assert(!sync_calls && !storage_finish(storage));
    storage_close(storage);
    puts("PASS: incoming page errors preserve the dirty victim and resident cache");
}

struct writer_args
{
    struct storage *storage;
    unsigned char *expected;
    pthread_barrier_t *barrier;
    size_t worker;
};

// Concurrent writers own disjoint regions while competing for bounded cache stripes.
static void *write_regions(void *data)
{
    struct writer_args *args=data;
    unsigned char actual[MODEL_SIZE], patch[73];
    pthread_barrier_wait(args->barrier);
    for (size_t region=args->worker; region < REGION_COUNT; region+=4)
    {
        unsigned char *expected=args->expected+region*MODEL_SIZE;
        fill_pattern(expected, 4096, region);
        assert(storage_write(args->storage, expected, 4096, region*REGION_SIZE)==4096);
        assert(storage_read(args->storage, actual, sizeof(actual), region*REGION_SIZE)==sizeof(actual));
        assert(!memcmp(actual, expected, sizeof(actual)));
        fill_pattern(patch, sizeof(patch), REGION_COUNT+region);
        assert(storage_write(args->storage, patch, sizeof(patch), region*REGION_SIZE+4096+123)==sizeof(patch));
        memcpy(expected+4096+123, patch, sizeof(patch));
    }
    return NULL;
}

// Race checkpoint barriers against cache pressure, then verify persistence after joining.
static void check_parallel(const char *path)
{
    unsigned char *expected=calloc(REGION_COUNT, MODEL_SIZE);
    assert(expected);
    struct storage *storage=open_store(path, NULL);
    pthread_t threads[4];
    pthread_barrier_t barrier;
    struct writer_args args[4];
    assert(!pthread_barrier_init(&barrier, NULL, 5));
    sync_calls=0;
    for (size_t i=0; i < 4; i++)
    {
        args[i]=(struct writer_args){storage, expected, &barrier, i};
        assert(!pthread_create(&threads[i], NULL, write_regions, &args[i]));
    }
    pthread_barrier_wait(&barrier);
    for (unsigned i=0; i < 16; i++)
    {
        assert(!storage_sync(storage));
        usleep(1000);
    }
    for (size_t i=0; i < 4; i++) assert(!pthread_join(threads[i], NULL));
    assert(!pthread_barrier_destroy(&barrier));
    check_regions(storage, expected, 1);
    assert(!storage_finish(storage));
    storage_close(storage);
    storage=open_store(path, NULL);
    check_regions(storage, expected, 0);
    assert(!storage_finish(storage));
    storage_close(storage);
    free(expected);
    puts("PASS: concurrent cache pressure, checkpoint barriers and remount persistence");
}

// Keep test storage isolated beneath a disposable directory owned by the runner.
int main(int argc, char **argv)
{
    if (argc!=2) { fprintf(stderr, "usage: %s temporary-directory\n", argv[0]); return 2; }
    char directory[PATH_MAX], path[PATH_MAX];
    assert(snprintf(directory, sizeof(directory), "%s/cache-XXXXXX", argv[1]) < (int)sizeof(directory));
    assert(mkdtemp(directory));
    assert(snprintf(path, sizeof(path), "%s/pressure", directory) < (int)sizeof(path)); check_pressure(path);
    assert(snprintf(path, sizeof(path), "%s/fail-write", directory) < (int)sizeof(path)); check_eviction_fault(path, 0);
    assert(snprintf(path, sizeof(path), "%s/short-write", directory) < (int)sizeof(path)); check_eviction_fault(path, 17);
    puts("PASS: failed/short dirty eviction retains RAM and retries complete index writes");
    assert(snprintf(path, sizeof(path), "%s/fail-read", directory) < (int)sizeof(path)); check_read_fault(path);
    assert(snprintf(path, sizeof(path), "%s/parallel", directory) < (int)sizeof(path)); check_parallel(path);
    return 0;
}
