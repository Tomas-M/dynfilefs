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

// Check complete recovery, torn tails, stale records and bounded malformed metadata.
int main(int argc, char **argv)
{
    assert(argc==2);
    char directory[PATH_MAX], path[PATH_MAX];
    assert(snprintf(directory, sizeof(directory), "%s/journal-XXXXXX", argv[1]) < (int)sizeof(directory));
    assert(mkdtemp(directory));
    assert(snprintf(path, sizeof(path), "%s/store", directory) < (int)sizeof(path));
    uint64_t root[2]={1, 0}, bits=3, previous=bits, length=0;
    size_t dirty[2]={0, 1};
    struct snapshot snapshot={4*1024*1024, 1024*1024, 4, 2, root, &bits, 7};
    assert(!snapshot_publish(path, &snapshot));
    for (int i=0; i < 3; i++)
    {
        snapshot.sequence++;
        snapshot.slots=(uint64_t)(i+1)*4;
        root[0]=(uint64_t)i*4+3;
        root[1]=i ? (uint64_t)i*4+1 : 0;
        bits=UINT64_C(15)<<(i*4);
        assert(!journal_append(path, &snapshot, &previous, 1, 2, dirty, 2, &length));
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
    assert(loaded.sequence==10 && loaded.root[0]==11 && loaded.root[1]==9 && loaded.bits[0]==0xf00);
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
        if (malformed==2) change_field(changed, 64, UINT64_MAX);
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
    assert(journal_append(path, &snapshot, &bits, 1, 2, dirty, 2, &length)==-E2BIG);
    assert(length==JOURNAL_LIMIT);
    length=0;
    snapshot.size=(JOURNAL_LIMIT/8+3)*4096*512;
    snapshot.root_count=(size_t)(snapshot.size/(4096*512));
    assert(journal_append(path, &snapshot, &bits, 1, 2, NULL, 0, &length)==-E2BIG);
    assert(!length && !journal_reset(path));
    fd=open_suffix(path, ".log", O_RDONLY);
    assert(fd==-ENOENT && !journal_reset(path));
    puts("PASS: journal replay, torn tail, interior damage, sequence gaps, compaction and growth bounds");
    return 0;
}
