#define _GNU_SOURCE
#include "durable.h"
#include "format.h"
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

struct transaction
{
    char magic[8];
    uint64_t length, checksum, modified, removed, padding;
    char name[512];
};

// Resolve owned relative names without following substituted directories or targets.
static int target_parent(int directory, const char *name, char base[NAME_MAX+1])
{
    if (!*name || (*name=='/' && directory!=AT_FDCWD) || strlen(name)>=PATH_MAX) return -EINVAL;
    int absolute=*name=='/';
    char path[PATH_MAX]; strcpy(path, name+absolute);
    int fd=openat(directory, absolute ? "/" : ".", O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    if (fd < 0) return -errno;
    char *part=path, *slash;
    while ((slash=strchr(part, '/')))
    {
        *slash=0;
        if (!*part || !strcmp(part, ".") || !strcmp(part, "..")) { close(fd); return -EINVAL; }
        int child=openat(fd, part, O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        int error=-errno; close(fd);
        if (child < 0) return error;
        fd=child; part=slash+1;
    }
    if (!*part || strlen(part)>NAME_MAX || !strcmp(part, ".") || !strcmp(part, "..")) { close(fd); return -EINVAL; }
    strcpy(base, part);
    return fd;
}

// Keep old allocation alive until its removed name is durably absent, including on FAT.
static int remove_entry(int directory, const char *name, int is_directory)
{
    char base[NAME_MAX+1]; int parent=target_parent(directory, name, base);
    if (parent < 0) return parent;
    int fd=openat(parent, base, O_RDONLY|O_NONBLOCK|O_NOFOLLOW|O_CLOEXEC|(is_directory ? O_DIRECTORY : 0)), result=0;
    if (fd < 0) { result=errno==ENOENT ? 0 : -errno; close(parent); return result; }
    struct stat st;
    if (fstat(fd, &st)) result=-errno;
    else if (is_directory ? !S_ISDIR(st.st_mode) : !S_ISREG(st.st_mode)) result=-EINVAL;
    if (!result && unlinkat(parent, base, is_directory ? AT_REMOVEDIR : 0)) result=-errno;
    if (!result && fsync(parent)) result=-errno;
    if (close(fd) && !result) result=-errno;
    close(parent);
    return result;
}

// Keep removed file clusters allocated until their directory entry is durably absent.
int durable_unlink(int directory, const char *name)
{
    return remove_entry(directory, name, 0);
}

// Empty staging directories need the same namespace-before-allocation ordering on FAT.
int durable_rmdir(int directory, const char *name)
{
    return remove_entry(directory, name, 1);
}

// Never shrink a published file: padding preserves FAT chains across interrupted writes.
static int replace_bytes(int directory, const char *name, const void *bytes, size_t length, int padding, int64_t modified)
{
    char base[NAME_MAX+1]; int parent=target_parent(directory, name, base);
    if (parent < 0) return parent;
    int fd=openat(parent, base, O_RDWR|O_CREAT|O_NOFOLLOW|O_CLOEXEC, 0600), result=0;
    if (fd < 0) { result=-errno; close(parent); return result; }
    struct stat st;
    if (fstat(fd, &st)) result=-errno;
    else if (!S_ISREG(st.st_mode) || st.st_size < 0 || length>INT64_MAX) result=-EINVAL;
    unsigned char previous[4096], fill[4096];
    for (size_t offset=0; !result && offset<length;)
    {
        size_t count=length-offset < sizeof(previous) ? length-offset : sizeof(previous);
        int changed=(uint64_t)st.st_size<offset+count;
        if (!changed) { result=read_at(fd, previous, count, (off_t)offset); if (!result) changed=memcmp(previous, (const char *)bytes+offset, count)!=0; }
        if (!result && changed) result=write_at(fd, (const char *)bytes+offset, count, (off_t)offset);
        offset+=count;
    }
    if (padding>=0) memset(fill, padding, sizeof(fill));
    for (uint64_t offset=length; !result && padding>=0 && offset<(uint64_t)st.st_size;)
    {
        size_t count=(uint64_t)st.st_size-offset < sizeof(previous) ? (size_t)((uint64_t)st.st_size-offset) : sizeof(previous);
        result=read_at(fd, previous, count, (off_t)offset);
        if (!result && memcmp(previous, fill, count)) result=write_at(fd, fill, count, (off_t)offset);
        offset+=count;
    }
    if (!result && modified>=0)
    {
        struct timespec times[2]={{.tv_nsec=UTIME_OMIT}, {.tv_sec=modified}};
        if (st.st_mtim.tv_sec > modified) times[1]=st.st_mtim;
        if (futimens(fd, times)) result=-errno;
    }
    if (!result && fsync(fd)) result=-errno;
    if (close(fd) && !result) result=-errno;
    if (!result && fsync(parent)) result=-errno;
    close(parent);
    return result;
}

// One permanent lock serializes redo publication and recovery across processes.
static int transaction_lock(int directory, const char *prefix, int write)
{
    int fd=open_suffix(directory, prefix, ".txn.lock", write ? O_RDWR|O_CREAT : O_RDONLY);
    if (fd < 0) return fd;
    if (flock(fd, write ? LOCK_EX : LOCK_SH)) { int result=-errno; close(fd); return result; }
    return fd;
}

// A torn candidate is ignored; canonical files are touched only after its full fsync.
static int read_transaction(int directory, const char *prefix, const char *target, unsigned char **record, size_t *size)
{
    *record=NULL; *size=0;
    int fd=open_suffix(directory, prefix, ".txn", O_RDONLY);
    if (fd==-ENOENT) return 0;
    if (fd < 0) return fd;
    struct stat st; struct transaction header={0};
    int result=fstat(fd, &st) ? -errno : 0;
    if (!result && !S_ISREG(st.st_mode)) result=-EINVAL;
    if (!result && st.st_size<(off_t)sizeof(header)) { close(fd); return 0; }
    if (!result) result=read_at(fd, &header, sizeof(header), 0);
    uint64_t length=le64toh(header.length);
    int candidate=!result && !memcmp(header.magic, "DYNTXN02", 8);
    if (candidate && (length<4096 || length>SIZE_MAX || length>(uint64_t)st.st_size)) candidate=0;
    // A different target cannot affect this canonical file; avoid rereading large unrelated redo payloads.
    if (target && candidate && memchr(header.name,0,sizeof(header.name)) && strcmp(target,header.name)) candidate=0;
    unsigned char *bytes=candidate ? malloc((size_t)length) : NULL;
    if (candidate && !bytes) result=-ENOMEM;
    if (!result && candidate) result=read_at(fd, bytes, (size_t)length, 0);
    close(fd);
    if (result) { free(bytes); return result; }
    if (!candidate) return 0;
    struct transaction *saved=(struct transaction *)bytes;
    uint64_t checksum=le64toh(saved->checksum); saved->checksum=0;
    if (metadata_checksum(bytes, (size_t)length)!=checksum) { free(bytes); return 0; }
    saved->checksum=htole64(checksum);
    if (!memchr(saved->name, 0, sizeof(saved->name)) || le64toh(saved->removed)>1 ||
        (int64_t)le64toh(saved->padding)<-1 || (int64_t)le64toh(saved->padding)>255)
    { free(bytes); return -EIO; }
    char base[NAME_MAX+1]; int parent=target_parent(directory, saved->name, base);
    if (parent < 0) { free(bytes); return parent; }
    close(parent); *record=bytes; *size=(size_t)length;
    return 0;
}

// Recovery completes the last durable intent without replacing or truncating any inode.
static int recover_locked(int directory, const char *prefix)
{
    unsigned char *record; size_t size;
    int result=read_transaction(directory, prefix, NULL, &record, &size);
    if (result || !record) return result;
    struct transaction *header=(struct transaction *)record;
    if (le64toh(header->removed)) result=durable_unlink(directory, header->name);
    else result=replace_bytes(directory, header->name, record+4096, size-4096, (int)le64toh(header->padding), (int64_t)le64toh(header->modified));
    free(record);
    return result;
}

// A previous intent must finish before the next operation can overwrite its redo bytes.
static int publish(int directory, const char *prefix, const char *name, const void *bytes, size_t length, int padding, int64_t modified, int removed, int keep, int *published)
{
    if (published) *published=0;
    if (strlen(name)>=sizeof(((struct transaction *)0)->name) || length>SIZE_MAX-4096) return -ENAMETOOLONG;
    char base[NAME_MAX+1], reserved[PATH_MAX];
    int parent=target_parent(directory, name, base);
    if (parent<0) return parent;
    // Reject an already invalid target before recording a committed write intent.
    struct stat target;
    int checked=fstatat(parent,base,&target,AT_SYMLINK_NOFOLLOW), error=errno;
    close(parent);
    if (!checked && !S_ISREG(target.st_mode)) return -EINVAL;
    if (checked && error!=ENOENT) return -error;
    int result=storage_name(reserved, prefix, ".txn");
    if (!result && !strcmp(name, reserved)) return -EINVAL;
    if (!result) result=storage_name(reserved, prefix, ".txn.lock");
    if (result) return result;
    if (!strcmp(name, reserved)) return -EINVAL;
    int lock=transaction_lock(directory, prefix, 1);
    if (lock < 0) return lock;
    result=recover_locked(directory, prefix);
    unsigned char *record=calloc(1, length+4096);
    if (!record && !result) result=-ENOMEM;
    if (!result)
    {
        struct transaction *header=(struct transaction *)record;
        memcpy(header->magic, "DYNTXN02", 8); header->length=htole64(length+4096);
        header->modified=htole64((uint64_t)modified); header->removed=htole64(removed); header->padding=htole64((uint64_t)padding); strcpy(header->name, name);
        if (length) memcpy(record+4096, bytes, length);
        header->checksum=htole64(metadata_checksum(record, length+4096));
        char journal[PATH_MAX]; result=storage_name(journal, prefix, ".txn");
        if (!result)
        {
            result=replace_bytes(directory, journal, record, length+4096, -1, -1);
            if (published)
            {
                *published=!result;
                if (result)
                {
                    // A failed flush can still leave a complete new intent; retain its pins.
                    unsigned char *observed=NULL; size_t size=0;
                    int inspected=read_transaction(directory, prefix, NULL, &observed, &size);
                    *published=inspected || (observed && size==length+4096 && !memcmp(observed, record, size));
                    free(observed);
                }
            }
        }
        if (!result) result=removed ? durable_unlink(directory, name) : replace_bytes(directory, name, bytes, length, padding, modified);
        if (!result && !keep)
        {
            // Append-only logs may reuse this name only after their deletion intent is forgotten.
            int fd=open_suffix(directory, prefix, ".txn", O_RDWR);
            if (fd<0) result=fd;
            else
            {
                result=write_at(fd, "\0\0\0\0\0\0\0\0", 8, 0);
                if (!result && fsync(fd)) result=-errno;
                if (close(fd) && !result) result=-errno;
            }
        }
    }
    free(record); close(lock);
    return result;
}

// Keep a replayable copy until the replacement data and containing directory are durable.
int durable_write(int directory, const char *prefix, const char *name, const void *bytes, size_t length, int padding, int64_t modified, int *published)
{
    return publish(directory, prefix, name, bytes, length, padding, modified, 0, 1, published);
}

// Deleted policy files stay deleted after recovery, even if the namespace update was interrupted.
int durable_remove(int directory, const char *prefix, const char *name)
{
    return publish(directory, prefix, name, NULL, 0, -1, -1, 1, 1, NULL);
}

// Retire a checkpoint/log safely, then stop replaying deletion before its name can be reused.
int durable_reset(int directory, const char *prefix, const char *name)
{
    int fd=openat(directory, name, O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    if (fd<0 && errno==ENOENT) return 0;
    if (fd<0) return -errno;
    close(fd);
    return publish(directory, prefix, name, NULL, 0, -1, -1, 1, 0, NULL);
}

// Read-only callers can expose the committed redo candidate without changing the medium.
int durable_pending(int directory, const char *prefix, char name[PATH_MAX], unsigned char **bytes, size_t *length)
{
    *bytes=NULL; *length=0; *name=0;
    int lock=transaction_lock(directory, prefix, 0);
    if (lock < 0 && lock!=-ENOENT) return lock;
    unsigned char *record; size_t size;
    int result=read_transaction(directory, prefix, NULL, &record, &size);
    if (!result && record)
    {
        struct transaction *header=(struct transaction *)record; strcpy(name, header->name);
        if (le64toh(header->removed)) result=-ENOENT;
        else { memmove(record, record+4096, size-4096); *bytes=record; *length=size-4096; record=NULL; }
        free(record);
    }
    if (lock>=0) close(lock);
    return result;
}

// Load one regular file without inventing another transaction namespace.
static int read_bytes(int directory, const char *name, unsigned char **bytes, size_t *length)
{
    int result=0;
    char base[NAME_MAX+1]; int parent=target_parent(directory, name, base);
    int fd=parent < 0 ? parent : openat(parent, base, O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    if (parent>=0) close(parent);
    if (fd < 0) result=parent < 0 ? parent : -errno;
    else
    {
        struct stat st;
        if (fstat(fd, &st)) result=-errno;
        else if (!S_ISREG(st.st_mode) || st.st_size<0 || (uint64_t)st.st_size>SIZE_MAX-1) result=-EIO;
        if (!result)
        {
            *length=(size_t)st.st_size; *bytes=malloc(*length+1);
            if (!*bytes) result=-ENOMEM;
            else { result=read_at(fd, *bytes, *length, 0); (*bytes)[*length]=0; }
        }
        close(fd);
    }
    return result;
}

// Text and binary readers share the same committed view while publication holds the lock.
int durable_read(int directory, const char *prefix, const char *name, unsigned char **bytes, size_t *length)
{
    *bytes=NULL; *length=0;
    int lock=transaction_lock(directory, prefix, 0);
    if (lock < 0 && lock!=-ENOENT) return lock;
    unsigned char *record; size_t size;
    int result=read_transaction(directory, prefix, name, &record, &size);
    if (!result && record)
    {
        struct transaction *header=(struct transaction *)record;
        if (!strcmp(name, header->name))
        {
            if (le64toh(header->removed)) result=-ENOENT;
            else { memmove(record, record+4096, size-4096); *bytes=record; *length=size-4096; record=NULL; }
        }
        free(record);
    }
    if (!result && !*bytes) result=read_bytes(directory, name, bytes, length);
    if (lock>=0) close(lock);
    if (result) { free(*bytes); *bytes=NULL; *length=0; }
    return result;
}

// First-time read-only inspection creates nothing; writers recover under the permanent lock.
int durable_recover(int directory, const char *prefix)
{
    int probe=open_suffix(directory, prefix, ".txn", O_RDONLY);
    if (probe==-ENOENT) return 0;
    if (probe < 0) return probe;
    close(probe);
    int lock=transaction_lock(directory, prefix, 1);
    if (lock < 0) return lock;
    int result=recover_locked(directory, prefix); close(lock);
    return result;
}

// Boot and desktop policy updates reuse the same durability protocol as snapshot metadata.
int durable_command(int argc, char **argv)
{
    if (argc<4) return 1;
    int directory=-1; char *prefix=NULL;
    int result=storage_path(argv[2], &directory, &prefix);
    unsigned char *bytes=NULL; size_t length=0;
    if (!result && !strcmp(argv[3], "recover") && argc==4) result=durable_recover(directory, prefix);
    else if (!result && argc>=5)
    {
        const char *name=argv[4];
        if (!strcmp(argv[3], "read") && argc==5)
        {
            result=durable_read(directory, prefix, name, &bytes, &length);
            if (result==-ENOENT) result=0;
            if (!result && length && fwrite(bytes, 1, length, stdout)!=length) result=-EIO;
        }
        else if (!strcmp(argv[3], "unlink") && argc==5) result=durable_unlink(directory, name);
        else if (!strcmp(argv[3], "rmdir") && argc==5) result=durable_rmdir(directory, name);
        else if (!strcmp(argv[3], "clear") && argc==5) result=durable_remove(directory, prefix, name);
        else if (!strcmp(argv[3], "mark") && argc==6)
        {
            char *end; errno=0; long long modified=strtoll(argv[5], &end, 10);
            if (errno || !*argv[5] || *end || modified<0) result=-EINVAL;
            else result=durable_write(directory, prefix, name, NULL, 0, '\n', modified, NULL);
        }
        else if (!strcmp(argv[3], "write"))
        {
            for (int i=5;i<argc;i++) { if (strlen(argv[i])>SIZE_MAX-length-1) { result=-EOVERFLOW; break; } length+=strlen(argv[i])+1; }
            if (length>PTRDIFF_MAX-1) result=-EOVERFLOW;
            if (!result) bytes=malloc(length+1);
            if (!result && !bytes) result=-ENOMEM;
            if (!result)
            {
                size_t offset=0;
                for (int i=5;i<argc;i++) { size_t count=strlen(argv[i]); memcpy(bytes+offset, argv[i], count); offset+=count; bytes[offset++]='\n'; }
                result=durable_write(directory, prefix, name, bytes, length, '\n', -1, NULL);
            }
        }
        else if (!strcmp(argv[3], "copy") && argc==6)
        {
            int source=-1; char *file=NULL; result=storage_path(argv[5], &source, &file);
            if (!result) result=read_bytes(source, file, &bytes, &length);
            if (!result) result=durable_write(directory, prefix, name, bytes, length, -1, -1, NULL);
            if (source>=0) close(source);
            free(file);
        }
        else result=-EINVAL;
    }
    else if (!result) result=-EINVAL;
    if (directory>=0) close(directory);
    free(prefix); free(bytes);
    if (result) fprintf(stderr, "Cannot update durable metadata: %s\n", strerror(-result));
    return !!result;
}
