#define _GNU_SOURCE
#include "format.h"
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

struct disk_header
{
    char magic[8];
    uint64_t version, size, part_size, slots, checksum, sequence;
    uint64_t id, parent, created;
    char name[DYN_NAME_MAX+1];
};

// Read an entire metadata range, rejecting truncated files.
int read_at(int fd, void *buffer, size_t size, off_t offset)
{
    while (size)
    {
        ssize_t count=pread(fd, buffer, size, offset);
        if (count < 0 && errno==EINTR) continue;
        if (count <= 0) return count ? -errno : -EIO;
        buffer=(char *)buffer+count; offset+=count; size-=(size_t)count;
    }
    return 0;
}

// Finish short writes before publishing references to their contents.
int write_at(int fd, const void *buffer, size_t size, off_t offset)
{
    while (size)
    {
        ssize_t count=pwrite(fd, buffer, size, offset);
        if (count < 0 && errno==EINTR) continue;
        if (count <= 0) return count ? -errno : -EIO;
        buffer=(const char *)buffer+count; offset+=count; size-=(size_t)count;
    }
    return 0;
}

// Pin the containing directory before daemonization, rename or a root transition.
int storage_path(const char *path, int *directory, char **name)
{
    char parent[PATH_MAX];
    if (strlen(path) >= sizeof(parent)) return -ENAMETOOLONG;
    strcpy(parent, path);
    char *slash=strrchr(parent, '/'), *base=slash ? slash+1 : parent;
    if (!*base) return -EINVAL;
    char *filename=strdup(base);
    if (!filename) return -ENOMEM;
    if (slash) slash[slash==parent ? 1 : 0]=0;
    else strcpy(parent, ".");
    int fd=open(parent, O_PATH|O_DIRECTORY|O_CLOEXEC);
    if (fd < 0) { int error=-errno; free(filename); return error; }
    *directory=fd; *name=filename;
    return 0;
}

// Keep all companion names within the platform path limit.
int storage_name(char *name, const char *path, const char *suffix)
{
    return snprintf(name, PATH_MAX, "%s%s", path, suffix) >= PATH_MAX ? -ENAMETOOLONG : 0;
}

// Open an owned companion file without following a substituted symlink.
int open_suffix(int directory, const char *path, const char *suffix, int flags)
{
    char name[PATH_MAX];
    int result=storage_name(name, path, suffix);
    if (result) return result;
    int fd=openat(directory, name, flags|O_CLOEXEC|O_NOFOLLOW, 0600);
    return fd < 0 ? -errno : fd;
}

// An opener may retain the old inode while the previous owner removes its name.
static int current_lock(int directory, const char *path, int fd)
{
    char name[PATH_MAX];
    int result=storage_name(name, path, ".lock");
    if (result) return result;
    struct stat held, current;
    if (fstat(fd, &held)) return -errno;
    if (!held.st_nlink) return 0;
    if (fstatat(directory, name, &current, AT_SYMLINK_NOFOLLOW)) return errno==ENOENT ? 0 : -errno;
    return held.st_dev==current.st_dev && held.st_ino==current.st_ino;
}

// Verify the locked inode is still named before beginning any storage I/O.
int lock_storage(int directory, const char *path)
{
    for (;;)
    {
        int fd=open_suffix(directory, path, ".lock", O_RDWR|O_CREAT);
        if (fd < 0) return fd;
        int result=flock(fd, LOCK_EX|LOCK_NB) ? -errno : current_lock(directory, path, fd);
        if (result==1) return fd;
        close(fd);
        if (result < 0) return result;
    }
}

// Only a finished owner may remove its lock, after all other storage I/O has ended.
int unlock_storage(int directory, const char *path, int fd)
{
    int current=current_lock(directory, path, fd), result=current < 0 ? current : 0;
    if (current==1)
    {
        char name[PATH_MAX];
        storage_name(name, path, ".lock");
        if (unlinkat(directory, name, 0)) result=-errno;
        if (!result) result=sync_directory(directory, path);
    }
    else if (!current) result=-ESTALE;
    if (close(fd) && !result) result=-errno;
    return result;
}

// Persist file creation, replacement and removal in the storage directory.
int sync_directory(int directory, const char *path)
{
    char name[PATH_MAX];
    if (strlen(path) >= sizeof(name)) return -ENAMETOOLONG;
    strcpy(name, path);
    char *slash=strrchr(name, '/');
    if (slash) slash[slash==name ? 1 : 0]=0;
    else strcpy(name, ".");
    int fd=openat(directory, name, O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    if (fd < 0) return -errno;
    int result=fsync(fd) ? -errno : 0;
    close(fd);
    return result;
}

// Detect incomplete or damaged metadata snapshots, not malicious changes.
uint64_t metadata_checksum(const unsigned char *bytes, size_t size)
{
    uint64_t hash=UINT64_C(14695981039346656037);
    while (size--) hash=(hash^*bytes++)*UINT64_C(1099511628211);
    return hash;
}

// Derive metadata lengths without trusting counts supplied by a file.
int snapshot_length(const struct snapshot *snapshot, size_t *length)
{
    if (!memchr(snapshot->name, 0, sizeof(snapshot->name))) return -EINVAL;
    if (snapshot->id)
    {
        if (!snapshot->name[0] || snapshot->parent >= snapshot->id) return -EINVAL;
    }
    else if (snapshot->name[0] || snapshot->created) return -EINVAL;
    if (!snapshot->part_size || snapshot->part_size%4096 || snapshot->part_size > INT64_MAX) return -EINVAL;
    uint64_t capacity=snapshot->part_size/4096;
    if (capacity > (UINT64_MAX-1)/DYN_MAX_PARTS) return -EOVERFLOW;
    if (snapshot->slots > capacity*DYN_MAX_PARTS) return -EINVAL;
    // Deleted points keep their lineage and name, but no data or allocation references.
    if (!snapshot->size)
    {
        if (!snapshot->id || snapshot->slots) return -EINVAL;
        *length=4096;
        return 0;
    }
    if (snapshot->size%4096 || snapshot->size > INT64_MAX) return -EINVAL;
    uint64_t roots=(snapshot->size-1)/(4096*512)+1;
    if (snapshot->slots > UINT64_MAX-63) return -EOVERFLOW;
    uint64_t words=(snapshot->slots+63)/64;
    if (roots > (SIZE_MAX-4096)/8 || words > (SIZE_MAX-4096)/8-roots) return -EOVERFLOW;
    *length=4096+(size_t)(roots+words)*8;
    return 0;
}

// Check references after decoding a snapshot or replaying complete journal records.
int snapshot_validate(const struct snapshot *snapshot)
{
    size_t length;
    int result=snapshot_length(snapshot, &length);
    if (result) return result;
    if (!snapshot->size) return snapshot->root_count || snapshot->root || snapshot->bits ? -EIO : 0;
    if (snapshot->root_count!=(snapshot->size-1)/(4096*512)+1) return -EIO;
    if (!snapshot->root || (snapshot->slots && !snapshot->bits)) return -EIO;
    for (size_t i=0; i < snapshot->root_count; i++)
    {
        uint64_t ref=snapshot->root[i];
        if (ref && (ref > snapshot->slots || !(snapshot->bits[(ref-1)/64]&(UINT64_C(1)<<((ref-1)%64))))) return -EIO;
    }
    if (snapshot->slots%64 && snapshot->bits[snapshot->slots/64]>>(snapshot->slots%64)) return -EIO;
    return 0;
}

// Load the canonical little-endian root and allocation bitmap.
int snapshot_load(int fd, struct snapshot *snapshot)
{
    struct disk_header header;
    int result=read_at(fd, &header, sizeof(header), 0);
    if (result) return result;
    if (memcmp(header.magic, DYN_MAGIC, 8) || le64toh(header.version)!=DYN_FORMAT) return -EPROTONOSUPPORT;
    snapshot->size=le64toh(header.size);
    snapshot->part_size=le64toh(header.part_size);
    snapshot->slots=le64toh(header.slots);
    snapshot->sequence=le64toh(header.sequence);
    snapshot->id=le64toh(header.id);
    snapshot->parent=le64toh(header.parent);
    snapshot->created=le64toh(header.created);
    memcpy(snapshot->name, header.name, sizeof(snapshot->name));
    size_t length;
    result=snapshot_length(snapshot, &length);
    if (result) return result;
    struct stat st;
    if (fstat(fd, &st)) return -errno;
    if ((uint64_t)st.st_size!=length) return -EIO;
    unsigned char *bytes=malloc(length);
    if (!bytes) return -ENOMEM;
    result=read_at(fd, bytes, length, 0);
    if (result) { free(bytes); return result; }
    memset(bytes+offsetof(struct disk_header, checksum), 0, 8);
    if (metadata_checksum(bytes, length)!=le64toh(header.checksum)) { free(bytes); return -EIO; }
    snapshot->root_count=snapshot->size ? (size_t)((snapshot->size-1)/(4096*512)+1) : 0;
    size_t words=(size_t)((snapshot->slots+63)/64);
    snapshot->root=snapshot->root_count ? malloc(snapshot->root_count*8) : NULL;
    snapshot->bits=words ? malloc(words*8) : NULL;
    if ((snapshot->root_count && !snapshot->root) || (words && !snapshot->bits)) { free(bytes); snapshot_free(snapshot); return -ENOMEM; }
    uint64_t *entries=(uint64_t *)(bytes+4096);
    for (size_t i=0; i < snapshot->root_count; i++) snapshot->root[i]=le64toh(entries[i]);
    for (size_t i=0; i < words; i++) snapshot->bits[i]=le64toh(entries[snapshot->root_count+i]);
    free(bytes);
    result=snapshot_validate(snapshot);
    if (result) snapshot_free(snapshot);
    return result;
}

// Write a complete checksummed candidate; the caller publishes it after fsync.
int snapshot_write(int fd, const struct snapshot *snapshot)
{
    size_t length;
    int result=snapshot_length(snapshot, &length);
    if (result) return result;
    if (!snapshot->size && (snapshot->root_count || snapshot->root || snapshot->bits)) return -EINVAL;
    unsigned char *bytes=calloc(1, length);
    if (!bytes) return -ENOMEM;
    struct disk_header *header=(struct disk_header *)bytes;
    memcpy(header->magic, DYN_MAGIC, 8);
    header->version=htole64(DYN_FORMAT);
    header->size=htole64(snapshot->size);
    header->part_size=htole64(snapshot->part_size);
    header->slots=htole64(snapshot->slots);
    header->sequence=htole64(snapshot->sequence);
    header->id=htole64(snapshot->id);
    header->parent=htole64(snapshot->parent);
    header->created=htole64(snapshot->created);
    memcpy(header->name, snapshot->name, strlen(snapshot->name)+1);
    size_t roots=snapshot->size ? (size_t)((snapshot->size-1)/(4096*512)+1) : 0;
    size_t words=(size_t)((snapshot->slots+63)/64);
    uint64_t *entries=(uint64_t *)(bytes+4096);
    for (size_t i=0; i < roots; i++) entries[i]=htole64(snapshot->root[i]);
    for (size_t i=0; i < words; i++) entries[roots+i]=htole64(snapshot->bits[i]);
    header->checksum=htole64(metadata_checksum(bytes, length));
    result=write_at(fd, bytes, length, 0);
    free(bytes);
    if (!result && ftruncate(fd, (off_t)length)) result=-errno;
    if (!result && fsync(fd)) result=-errno;
    return result;
}

// Replace a root/bitmap pair; report publication even if the directory flush fails.
int snapshot_replace(int directory, const char *path, const char *suffix, const struct snapshot *snapshot, int *published)
{
    if (published) *published=0;
    int fd=open_suffix(directory, path, ".next", O_RDWR|O_CREAT|O_TRUNC);
    if (fd < 0) return fd;
    int result=snapshot_write(fd, snapshot);
    if (close(fd) && !result) result=-errno;
    char name[PATH_MAX], target[PATH_MAX];
    if (!result) result=storage_name(name, path, ".next");
    if (!result) result=storage_name(target, path, suffix);
    if (!result)
    {
        if (renameat(directory, name, directory, target)) result=-errno;
        else if (published) *published=1;
    }
    if (!result) result=sync_directory(directory, path);
    return result;
}

// Initial creation and conversion publish directly to the committed main file.
int snapshot_publish(int directory, const char *path, const struct snapshot *snapshot)
{
    return snapshot_replace(directory, path, "", snapshot, NULL);
}

// Release the decoded snapshot arrays.
void snapshot_free(struct snapshot *snapshot)
{
    free(snapshot->root); free(snapshot->bits);
    snapshot->root=NULL; snapshot->bits=NULL;
}
