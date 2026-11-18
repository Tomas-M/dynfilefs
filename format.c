#define _GNU_SOURCE
#include "format.h"
#include "durable.h"
#include <dirent.h>
#include <inttypes.h>
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

// Decode geometry before allocating memory or trusting counts from the medium.
static int snapshot_header(const struct disk_header *header, struct snapshot *snapshot, size_t *length)
{
    if (memcmp(header->magic, DYN_MAGIC, 8) || le64toh(header->version)!=DYN_FORMAT) return -EPROTONOSUPPORT;
    snapshot->size=le64toh(header->size); snapshot->part_size=le64toh(header->part_size);
    snapshot->slots=le64toh(header->slots); snapshot->sequence=le64toh(header->sequence);
    snapshot->id=le64toh(header->id); snapshot->parent=le64toh(header->parent);
    snapshot->created=le64toh(header->created); memcpy(snapshot->name, header->name, sizeof(snapshot->name));
    return snapshot_length(snapshot, length);
}

// Ignore allocation-preserving padding while validating the complete logical metadata.
int snapshot_decode(unsigned char *bytes, size_t available, struct snapshot *snapshot)
{
    if (available<sizeof(struct disk_header)) return -EIO;
    struct disk_header header; memcpy(&header, bytes, sizeof(header));
    size_t length; int result=snapshot_header(&header, snapshot, &length);
    if (result) return result;
    if (available<length) return -EIO;
    memset(bytes+offsetof(struct disk_header, checksum), 0, 8);
    uint64_t checksum=metadata_checksum(bytes, length);
    memcpy(bytes+offsetof(struct disk_header, checksum), &header.checksum, 8);
    if (checksum!=le64toh(header.checksum)) return -EIO;
    snapshot->root_count=snapshot->size ? (size_t)((snapshot->size-1)/(4096*512)+1) : 0;
    size_t words=(size_t)((snapshot->slots+63)/64);
    snapshot->root=snapshot->root_count ? malloc(snapshot->root_count*8) : NULL;
    snapshot->bits=words ? malloc(words*8) : NULL;
    if ((snapshot->root_count && !snapshot->root) || (words && !snapshot->bits)) { snapshot_free(snapshot); return -ENOMEM; }
    uint64_t *entries=(uint64_t *)(bytes+4096);
    for (size_t i=0; i<snapshot->root_count; i++) snapshot->root[i]=le64toh(entries[i]);
    for (size_t i=0; i<words; i++) snapshot->bits[i]=le64toh(entries[snapshot->root_count+i]);
    result=snapshot_validate(snapshot);
    if (result) snapshot_free(snapshot);
    return result;
}

// Read only the logical record, keeping large preallocated tails out of memory.
int snapshot_load(int fd, struct snapshot *snapshot)
{
    struct disk_header header; int result=read_at(fd, &header, sizeof(header), 0);
    size_t length;
    if (!result) result=snapshot_header(&header, snapshot, &length);
    if (result) return result;
    struct stat st;
    if (fstat(fd, &st)) return -errno;
    if (!S_ISREG(st.st_mode) || st.st_size<0 || (uint64_t)st.st_size<length) return -EIO;
    unsigned char *bytes=malloc(length);
    if (!bytes) return -ENOMEM;
    result=read_at(fd, bytes, length, 0);
    if (!result) result=snapshot_decode(bytes, length, snapshot);
    free(bytes);
    return result;
}

// Offline readers see committed redo records without publishing recovery writes.
int snapshot_read_at(int directory, const char *path, const char *suffix, struct snapshot *snapshot)
{
    char name[PATH_MAX]; int result=storage_name(name, path, suffix);
    unsigned char *bytes=NULL; size_t length=0;
    if (!result) result=durable_read(directory, path, name, &bytes, &length);
    if (!result) result=snapshot_decode(bytes, length, snapshot);
    free(bytes);
    return result;
}

// Serialize one checksummed logical image for direct fixtures and durable publication.
static int snapshot_encode(const struct snapshot *snapshot, unsigned char **encoded, size_t *size)
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
    *encoded=bytes; *size=length;
    return 0;
}

// Direct callers own an unpublished file; published records use the redo protocol.
int snapshot_write(int fd, const struct snapshot *snapshot)
{
    unsigned char *bytes=NULL; size_t length=0;
    int result=snapshot_encode(snapshot, &bytes, &length);
    if (!result) result=write_at(fd, bytes, length, 0);
    free(bytes);
    if (!result && ftruncate(fd, (off_t)length)) result=-errno;
    if (!result && fsync(fd)) result=-errno;
    return result;
}

// Keep a complete durable redo image before touching the canonical inode.
int snapshot_replace(int directory, const char *path, const char *suffix, const struct snapshot *snapshot, int *published)
{
    if (directory==AT_FDCWD)
    {
        // Store redo names relative to the same pinned parent used by later mounts.
        int parent=-1; char *base=NULL;
        int result=storage_path(path, &parent, &base);
        if (!result) result=snapshot_replace(parent, base, suffix, snapshot, published);
        if (parent>=0) close(parent);
        free(base); return result;
    }
    if (published) *published=0;
    unsigned char *bytes=NULL; size_t length=0; char name[PATH_MAX];
    int result=storage_name(name, path, suffix);
    if (!result) result=snapshot_encode(snapshot, &bytes, &length);
    if (!result) result=durable_write(directory, path, name, bytes, length, -1, -1, published);
    free(bytes);
    return result;
}

// Append unique stable IDs while retaining linear directory-scan complexity.
static int add_snapshot_id(uint64_t id, uint64_t **ids, size_t *count, size_t *capacity)
{
    if (*count==*capacity)
    {
        size_t grown=*capacity ? *capacity*2 : 16;
        if (grown<*capacity || grown>SIZE_MAX/sizeof(**ids)) return -EOVERFLOW;
        uint64_t *next=realloc(*ids, grown*sizeof(**ids));
        if (!next) return -ENOMEM;
        *ids=next; *capacity=grown;
    }
    (*ids)[(*count)++]=id;
    return 0;
}

// Include a newly committed point whose directory entry was interrupted before publication.
int snapshot_ids(int directory, const char *path, uint64_t **ids, size_t *count)
{
    *ids=NULL; *count=0;
    char folder[PATH_MAX]; int result=storage_name(folder, path, ".snapshots");
    if (result) return result;
    int fd=openat(directory, folder, O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if (fd<0) return errno==ENOENT ? 0 : -errno;
    DIR *dir=fdopendir(fd);
    if (!dir) { result=-errno; close(fd); return result; }
    size_t capacity=0; struct dirent *entry;
    for (errno=0; (entry=readdir(dir)); errno=0)
    {
        size_t length=strlen(entry->d_name);
        if (length<5 || strcmp(entry->d_name+length-5, ".snap")) continue;
        uint64_t id=0; char name[32];
        if (length!=21 || sscanf(entry->d_name, "%16" SCNx64, &id)!=1) { result=-EIO; break; }
        snprintf(name, sizeof(name), "%016" PRIx64 ".snap", id);
        if (!id || strcmp(name, entry->d_name)) { result=-EIO; break; }
        result=add_snapshot_id(id, ids, count, &capacity);
        if (result) break;
    }
    if (!result && errno) result=-errno;
    closedir(dir);
    unsigned char *bytes=NULL; size_t length=0; char pending[PATH_MAX];
    if (!result) result=durable_pending(directory, path, pending, &bytes, &length);
    if (result==-ENOENT) result=0;
    size_t prefix=strlen(folder);
    if (!result && bytes && !strncmp(pending, folder, prefix) && pending[prefix]=='/')
    {
        struct snapshot saved={0}; result=snapshot_decode(bytes, length, &saved);
        char suffix[64], name[PATH_MAX]; snprintf(suffix, sizeof(suffix), ".snapshots/%016" PRIx64 ".snap", saved.id);
        if (!result) result=storage_name(name, path, suffix);
        if (!result && (!saved.id || strcmp(name, pending))) result=-EIO;
        int found=0;
        for (size_t i=0; i<*count; i++) if ((*ids)[i]==saved.id) found=1;
        if (!result && !found) result=add_snapshot_id(saved.id, ids, count, &capacity);
        snapshot_free(&saved);
    }
    free(bytes);
    if (result) { free(*ids); *ids=NULL; *count=0; }
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
