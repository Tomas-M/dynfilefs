#define _GNU_SOURCE
#include "journal.h"
#include "report.h"
#include <dirent.h>
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

// A shared lock on an existing lock file never creates or publishes storage state.
static int inspect_lock(const char *path, int *lock)
{
    *lock=open_suffix(path, ".lock", O_RDONLY);
    if (*lock==-ENOENT) { *lock=-1; return 0; }
    if (*lock < 0) return *lock;
    if (flock(*lock, LOCK_SH|LOCK_NB)) return errno==EWOULDBLOCK ? -EBUSY : -errno;
    char name[PATH_MAX];
    int result=storage_name(name, path, ".lock");
    struct stat held, named;
    if (!result && (fstat(*lock, &held) || lstat(name, &named))) result=errno==ENOENT ? -EAGAIN : -errno;
    if (!result && (held.st_dev!=named.st_dev || held.st_ino!=named.st_ino)) result=-EAGAIN;
    return result;
}

// Watch the containing directory too: a short mount may create and remove its lock between checks.
static int info_files(const char *path, struct stat files[5])
{
    const char *suffixes[]={"", ".work", ".log", ".snapshots"};
    memset(files, 0, 5*sizeof(*files));
    char name[PATH_MAX];
    for (size_t i=0; i < 4; i++)
    {
        int result=storage_name(name, path, suffixes[i]);
        if (result) return result;
        if (lstat(name, &files[i]) && errno!=ENOENT) return -errno;
    }
    int result=storage_name(name, path, "");
    if (result) return result;
    char *slash=strrchr(name, '/');
    if (slash) slash[slash==name ? 1 : 0]=0;
    else strcpy(name, ".");
    return stat(name, &files[4]) ? -errno : 0;
}

// Merge occupancy without the live allocator's caches and free-run indexes.
static int merge_pins(uint64_t **bits, size_t *words, const struct snapshot *snapshot)
{
    uint64_t required=(snapshot->slots+63)/64;
    if (required > SIZE_MAX/8) return -EOVERFLOW;
    if (required > *words)
    {
        uint64_t *grown=realloc(*bits, (size_t)required*8);
        if (!grown) return -ENOMEM;
        memset(grown+*words, 0, ((size_t)required-*words)*8);
        *bits=grown; *words=(size_t)required;
    }
    for (size_t i=0; i < required; i++) (*bits)[i]|=snapshot->bits[i];
    return 0;
}

// Count only occupied slots within one existing physical file's extent.
static uint64_t range_pins(const uint64_t *bits, size_t words, uint64_t first, uint64_t count)
{
    uint64_t occupied=0;
    while (count && first/64 < words)
    {
        unsigned length=(unsigned)(count < 64-first%64 ? count : 64-first%64);
        uint64_t mask=(UINT64_MAX>>(64-length))<<(first%64);
        occupied+=__builtin_popcountll(bits[first/64]&mask);
        first+=length; count-=length;
    }
    return occupied;
}

// Physical holes between numbered files are never counted as reusable capacity.
static int reusable_parts(const char *path, const struct snapshot *current, const uint64_t *bits, size_t words, uint64_t *reusable)
{
    char directory[PATH_MAX], prefix[PATH_MAX];
    int result=storage_name(directory, path, "");
    if (result) return result;
    char *slash=strrchr(directory, '/');
    const char *base=slash ? slash+1 : directory;
    if (snprintf(prefix, sizeof(prefix), "%s.", base) >= (int)sizeof(prefix)) return -ENAMETOOLONG;
    if (slash) slash[slash==directory ? 1 : 0]=0;
    else strcpy(directory, ".");
    DIR *dir=opendir(directory);
    if (!dir) return -errno;
    uint64_t total=0, backed=0, capacity=current->part_size/4096;
    for (size_t i=0; i < words; i++) total+=__builtin_popcountll(bits[i]);
    struct dirent *entry;
    *reusable=0;
    for (errno=0; (entry=readdir(dir)); errno=0)
    {
        if (strncmp(entry->d_name, prefix, strlen(prefix))) continue;
        const char *number=entry->d_name+strlen(prefix);
        if (*number < '0' || *number > '9') continue;
        char *end;
        errno=0;
        unsigned long index=strtoul(number, &end, 10);
        if (*end || (number[0]=='0' && number[1])) continue;
        if (errno || index >= DYN_MAX_PARTS) { result=-EFBIG; break; }
        char suffix[32];
        snprintf(suffix, sizeof(suffix), ".%lu", index);
        int fd=open_suffix(path, suffix, O_RDONLY);
        if (fd < 0) { result=fd; break; }
        struct stat st;
        result=fstat(fd, &st) ? -errno : 0;
        close(fd);
        if (!result && (!S_ISREG(st.st_mode) || st.st_size < 0 || (uint64_t)st.st_size > current->part_size)) result=-EIO;
        if (result) break;
        uint64_t slots=((uint64_t)st.st_size+4095)/4096;
        uint64_t occupied=range_pins(bits, words, (uint64_t)index*capacity, slots);
        backed+=occupied;
        if (slots-occupied > (UINT64_MAX-*reusable)/4096) { result=-EOVERFLOW; break; }
        *reusable+=(slots-occupied)*4096;
    }
    if (!result && !entry && errno) result=-errno;
    closedir(dir);
    if (!result && backed!=total) result=-EIO;
    return result;
}

// Read legacy geometry conservatively; conversion determines reusable capacity on its first mount.
static int inspect_legacy(int fd, struct storage_info *info)
{
    uint64_t header[3];
    char magic[8];
    struct stat file;
    int result=fstat(fd, &file) ? -errno : 0;
    if (!result && file.st_size < 4096) result=-EIO;
    if (!result) result=read_at(fd, magic, sizeof(magic), 0);
    if (!result && !memcmp(magic, "DYNFS", 5)) result=-EPROTONOSUPPORT;
    if (!result) result=read_at(fd, header, sizeof(header), 2048);
    if (!result && le64toh(header[0])!=500) result=-EPROTONOSUPPORT;
    if (!result)
    {
        info->version=500; info->split=le64toh(header[1]); info->size=le64toh(header[2]);
        if (!info->size || info->size > INT64_MAX || info->size%4096) result=-EINVAL;
        if (!info->split || info->split > INT64_MAX || info->split%4096) result=-EINVAL;
    }
    return result;
}

// Recover metadata in memory only, keeping inspection free of automatic snapshots and disk writes.
static int inspect_state(const char *path, struct storage_info *info, int json, char **report)
{
    struct snapshot current={0}, saved={0};
    struct snapshot_record *records=NULL;
    size_t count=0, capacity=0, words=0;
    uint64_t *bits=NULL;
    int fd=open_suffix(path, "", O_RDONLY);
    if (fd < 0) return fd;
    int result=snapshot_load(fd, &current);
    if (result==-EPROTONOSUPPORT)
    {
        result=inspect_legacy(fd, info);
        close(fd);
        if (!result && report) result=snapshot_report(NULL, 0, 0, json, report);
        return result;
    }
    close(fd);
    if (result) goto out;
    if (current.id || !current.size) { result=-EIO; goto out; }
    fd=open_suffix(path, ".work", O_RDONLY);
    if (fd >= 0)
    {
        result=snapshot_load(fd, &saved);
        close(fd);
        if (!result && (saved.id || !saved.size || saved.part_size!=current.part_size)) result=-EIO;
        if (!result && saved.sequence > current.sequence)
        {
            if (saved.size < current.size) result=-EIO;
            else { snapshot_free(&current); current=saved; memset(&saved, 0, sizeof(saved)); }
        }
        snapshot_free(&saved);
    }
    else if (fd!=-ENOENT) result=fd;
    int replayed;
    if (!result) result=journal_replay(path, &current, &replayed);
    if (!result) result=merge_pins(&bits, &words, &current);
    if (result) goto out;
    char name[PATH_MAX];
    result=storage_name(name, path, ".snapshots");
    if (result) goto out;
    fd=open(name, O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if (fd < 0 && errno!=ENOENT) { result=-errno; goto out; }
    if (fd >= 0)
    {
        DIR *dir=fdopendir(fd);
        if (!dir) { result=-errno; close(fd); goto out; }
        struct dirent *entry;
        for (errno=0; (entry=readdir(dir)); errno=0)
        {
            size_t length=strlen(entry->d_name);
            if (length < 5 || strcmp(entry->d_name+length-5, ".snap")) continue;
            uint64_t id=0;
            char filename[32];
            if (length!=21 || sscanf(entry->d_name, "%16" SCNx64, &id)!=1) { result=-EIO; break; }
            snprintf(filename, sizeof(filename), "%016" PRIx64 ".snap", id);
            if (!id || strcmp(filename, entry->d_name)) { result=-EIO; break; }
            int source=openat(dirfd(dir), filename, O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
            if (source < 0) { result=-errno; break; }
            result=snapshot_load(source, &saved);
            close(source);
            if (!result && (saved.id!=id || saved.part_size!=current.part_size)) result=-EIO;
            if (!result) result=merge_pins(&bits, &words, &saved);
            if (!result && count==capacity)
            {
                size_t grown=capacity ? capacity*2 : 16;
                if (grown < capacity || grown > SIZE_MAX/sizeof(*records)) result=-EOVERFLOW;
                else
                {
                    struct snapshot_record *allocated=realloc(records, grown*sizeof(*records));
                    if (!allocated) result=-ENOMEM;
                    else { records=allocated; capacity=grown; }
                }
            }
            if (!result)
            {
                char *label=strdup(saved.name);
                if (!label) result=-ENOMEM;
                else records[count++]=(struct snapshot_record){saved.id, saved.parent, saved.created, saved.size, label};
            }
            snapshot_free(&saved);
            if (result) break;
        }
        if (!result && !entry && errno) result=-errno;
        closedir(dir);
    }
    if (!result && current.parent)
    {
        size_t i=0;
        while (i < count && records[i].id!=current.parent) i++;
        if (i==count) result=-EIO;
    }
    if (!result) result=reusable_parts(path, &current, bits, words, &info->reusable);
    if (!result)
    {
        info->version=DYN_FORMAT; info->size=current.size; info->split=current.part_size; info->parent=current.parent;
        if (report) result=snapshot_report(records, count, current.parent, json, report);
    }
out:
    for (size_t i=0; i < count; i++) free((char *)records[i].name);
    free(records); free(bits);
    snapshot_free(&current); snapshot_free(&saved);
    return result;
}

// Reject an active mount and retry any publication observed during a read-only inspection.
static int inspect_storage(const char *path, struct storage_info *info, int json, char **report)
{
    for (int attempt=0; attempt < 3; attempt++)
    {
        int lock=-1, result=inspect_lock(path, &lock);
        if (result) { if (lock >= 0) close(lock); if (result==-EAGAIN) continue; return result; }
        struct stat before[5], after[5];
        result=info_files(path, before);
        if (result) { if (lock >= 0) close(lock); return result; }
        memset(info, 0, sizeof(*info));
        if (report) *report=NULL;
        result=inspect_state(path, info, json, report);
        int end_lock=-1, error=inspect_lock(path, &end_lock), changed=0;
        if (!error) error=info_files(path, after);
        if (end_lock >= 0) close(end_lock);
        if (lock >= 0) close(lock);
        if (!error)
            for (size_t i=0; i < 5; i++)
            {
                int same_file=before[i].st_dev==after[i].st_dev && before[i].st_ino==after[i].st_ino;
                int same_size=before[i].st_size==after[i].st_size;
                int same_mtime=before[i].st_mtim.tv_sec==after[i].st_mtim.tv_sec && before[i].st_mtim.tv_nsec==after[i].st_mtim.tv_nsec;
                int same_ctime=before[i].st_ctim.tv_sec==after[i].st_ctim.tv_sec && before[i].st_ctim.tv_nsec==after[i].st_ctim.tv_nsec;
                if (!same_file || !same_size || !same_mtime || !same_ctime) changed=1;
            }
        if (error || changed)
        {
            if (report) { free(*report); *report=NULL; }
            if (error && error!=-EAGAIN) return error;
            continue;
        }
        return result;
    }
    return -EAGAIN;
}

// Inspect exact post-recovery capacity without mounting or committing anything.
int storage_inspect(const char *path, struct storage_info *info)
{
    return inspect_storage(path, info, 0, NULL);
}

// Render the stored ancestry before choosing a mount-time restore target.
int storage_snapshot_inspect(const char *path, int json, char **result)
{
    struct storage_info info;
    return inspect_storage(path, &info, json, result);
}
