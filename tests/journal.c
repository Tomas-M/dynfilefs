#define _GNU_SOURCE
#include "../journal.h"
#include <assert.h>
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// Load fresh arrays so each replay begins from an independent durable snapshot.
static struct snapshot load_base(const char *path)
{
    struct snapshot snapshot={0};
    int fd=open(path, O_RDONLY);
    assert(fd >= 0 && !snapshot_load(fd, &snapshot));
    close(fd);
    return snapshot;
}

// Replace a log fixture without changing the committed base snapshot.
static void write_log(const char *path, const unsigned char *bytes, size_t size)
{
    int fd=open_suffix(path, ".log", O_WRONLY|O_CREAT|O_TRUNC);
    assert(fd >= 0 && !write_at(fd, bytes, size, 0));
    close(fd);
}

// Alter a checksummed field to exercise semantic validation beyond torn-write detection.
static void change_field(unsigned char *record, size_t offset, uint64_t value)
{
    uint64_t disk=htole64(value);
    memcpy(record+offset, &disk, 8);
    memset(record+16, 0, 8);
    disk=htole64(metadata_checksum(record, 4096));
    memcpy(record+16, &disk, 8);
}

// Dirty subsets must survive growth, implicit tail removal and reuse of the log name.
static void check_dirty_words(const char *path)
{
    uint64_t root[3]={1, 0, 0}, bits[6]={1, 2, 4, 8, 0, 0}, previous[6], length=0;
    size_t dirty_roots[2]={1, 2}, dirty_words[3]={5, 3, 1};
    struct snapshot snapshot={.size=4*1024*1024, .part_size=1024*1024, .slots=256, .root_count=2, .root=root, .bits=bits, .sequence=20};
    assert(!snapshot_publish(path, &snapshot));
    memcpy(previous, bits, sizeof(bits));
    int log_fd=-1, replayed;
    snapshot.sequence++; snapshot.size=6*1024*1024; snapshot.root_count=3; snapshot.slots=353;
    root[1]=353; bits[3]=0; bits[5]=UINT64_C(1)<<32;
    assert(!journal_append(path, &snapshot, previous, 4, 2, dirty_roots, 1, dirty_words, 3, &log_fd, &length));
    int first_fd=log_fd, reader=open_suffix(path, ".log", O_RDONLY);
    uint64_t changed;
    assert(reader >= 0 && !read_at(reader, &changed, 8, 56) && le64toh(changed)==2);
    close(reader);
    memcpy(previous, bits, sizeof(bits));
    snapshot.sequence++; snapshot.slots=131;
    root[1]=0; root[2]=131; bits[5]=0;
    dirty_words[1]=4;
    assert(!journal_append(path, &snapshot, previous, 6, 3, dirty_roots, 2, dirty_words, 3, &log_fd, &length));
    assert(log_fd==first_fd && fcntl(log_fd, F_GETFD)>=0);
    struct snapshot loaded=load_base(path);
    assert(!journal_replay(path, &loaded, &replayed) && replayed);
    assert(loaded.sequence==22 && loaded.slots==131 && loaded.root_count==3);
    assert(loaded.root[0]==1 && !loaded.root[1] && loaded.root[2]==131);
    assert(!memcmp(loaded.bits, bits, 3*8));
    snapshot_free(&loaded);
    memcpy(previous, bits, sizeof(bits));
    snapshot.sequence++; snapshot.slots=325;
    root[1]=325; bits[5]=16;
    assert(!journal_append(path, &snapshot, previous, 3, 3, dirty_roots, 1, dirty_words, 1, &log_fd, &length));
    loaded=load_base(path);
    assert(!journal_replay(path, &loaded, &replayed) && replayed);
    assert(loaded.sequence==23 && loaded.slots==325 && !memcmp(loaded.bits, bits, sizeof(bits)));
    snapshot_free(&loaded);

    // Keep the retired inode alive to prove subsequent appends target the replacement.
    int retired=dup(log_fd);
    struct stat old, current;
    assert(retired >= 0 && !fstat(retired, &old));
    assert(!journal_reset(path, &log_fd) && log_fd==-1);
    assert(fcntl(first_fd, F_GETFD)==-1 && errno==EBADF);
    assert(!fstat(retired, &current) && !current.st_nlink);
    assert(!snapshot_publish(path, &snapshot));
    snapshot.sequence++; length=0;
    assert(!journal_append(path, &snapshot, bits, 6, 3, NULL, 0, NULL, 0, &log_fd, &length));
    assert(!fstat(log_fd, &current) && (old.st_dev!=current.st_dev || old.st_ino!=current.st_ino));
    assert(!fstat(retired, &current) && current.st_size==old.st_size);
    close(retired);
    loaded=load_base(path);
    assert(!journal_replay(path, &loaded, &replayed) && replayed && loaded.sequence==24);
    snapshot_free(&loaded);
    assert(!close(log_fd));
    assert(journal_reset(path, &log_fd)==-EBADF && log_fd==-1);
    reader=open_suffix(path, ".log", O_RDONLY);
    assert(reader >= 0);
    close(reader);
    assert(!journal_reset(path, &log_fd));
}

// Check complete recovery, torn tails, stale records and bounded malformed metadata.
int main(int argc, char **argv)
{
    assert(argc==2);
    char directory[PATH_MAX], path[PATH_MAX];
    assert(snprintf(directory, sizeof(directory), "%s/journal-XXXXXX", argv[1]) < (int)sizeof(directory));
    assert(mkdtemp(directory));
    assert(snprintf(path, sizeof(path), "%s/store", directory) < (int)sizeof(path));
    uint64_t root[2]={1, 0}, bits=3, previous=bits, length=0;
    size_t dirty[2]={0, 1}, dirty_word=0;
    int log_fd=-1;
    struct snapshot snapshot={.size=4*1024*1024, .part_size=1024*1024, .slots=4, .root_count=2, .root=root, .bits=&bits, .sequence=7};
    assert(!snapshot_publish(path, &snapshot));
    for (int i=0; i < 3; i++)
    {
        snapshot.sequence++;
        snapshot.parent=snapshot.sequence;
        snapshot.slots=(uint64_t)(i+1)*4;
        root[0]=(uint64_t)i*4+3;
        root[1]=i ? (uint64_t)i*4+1 : 0;
        bits=UINT64_C(15)<<(i*4);
        assert(!journal_append(path, &snapshot, &previous, 1, 2, dirty, 2, &dirty_word, 1, &log_fd, &length));
        assert(fcntl(log_fd, F_GETFD)>=0);
        previous=bits;
    }
    assert(length==3*4096);
    unsigned char bytes[3*4096], changed[3*4096];
    int fd=open_suffix(path, ".log", O_RDONLY);
    assert(fd >= 0 && !read_at(fd, bytes, sizeof(bytes), 0));
    close(fd);
    int replayed;
    struct snapshot loaded=load_base(path);
    assert(loaded.sequence==7 && !journal_replay(path, &loaded, &replayed) && replayed);
    assert(loaded.parent==10 && loaded.sequence==10 && loaded.root[0]==11 && loaded.root[1]==9 && loaded.bits[0]==0xf00);
    snapshot_free(&loaded);
    for (int torn=0; torn < 2; torn++)
    {
        memcpy(changed, bytes, sizeof(bytes));
        if (torn) changed[2*4096+100]^=1;
        write_log(path, changed, sizeof(changed)-(torn ? 0 : 17));
        loaded=load_base(path);
        assert(!journal_replay(path, &loaded, &replayed) && replayed);
        assert(loaded.sequence==9 && loaded.root[0]==7 && loaded.root[1]==5 && loaded.bits[0]==0xf0);
        snapshot_free(&loaded);
    }
    for (int malformed=0; malformed < 6; malformed++)
    {
        memcpy(changed, bytes, sizeof(bytes));
        if (!malformed) changed[100]^=1;
        if (malformed==1) change_field(changed+4096, 24, 10);
        if (malformed==2) change_field(changed, 72, UINT64_MAX);
        if (malformed==3) change_field(changed, 48, UINT64_MAX);
        if (malformed==4) change_field(changed, 32, (JOURNAL_LIMIT/8+3)*4096*512);
        if (malformed==5) change_field(changed, 40, UINT64_MAX);
        write_log(path, changed, sizeof(changed));
        loaded=load_base(path);
        assert(journal_replay(path, &loaded, &replayed)<0);
        snapshot_free(&loaded);
    }
    write_log(path, bytes, sizeof(bytes));
    assert(!snapshot_publish(path, &snapshot));
    loaded=load_base(path);
    assert(!journal_replay(path, &loaded, &replayed) && !replayed && loaded.sequence==10);
    snapshot_free(&loaded);
    length=JOURNAL_LIMIT;
    assert(journal_append(path, &snapshot, &bits, 1, 2, dirty, 2, &dirty_word, 1, &log_fd, &length)==-E2BIG);
    assert(length==JOURNAL_LIMIT);
    length=0;
    snapshot.size=(JOURNAL_LIMIT/8+3)*4096*512;
    snapshot.root_count=(size_t)(snapshot.size/(4096*512));
    assert(journal_append(path, &snapshot, &bits, 1, 2, NULL, 0, NULL, 0, &log_fd, &length)==-E2BIG);
    assert(!length && !journal_reset(path, &log_fd) && log_fd==-1);
    fd=open_suffix(path, ".log", O_RDONLY);
    assert(fd==-ENOENT && !journal_reset(path, &log_fd));
    check_dirty_words(path);
    puts("PASS: journal recovery, dirty word subsets, growth/tails and persistent descriptor reset");
    return 0;
}
