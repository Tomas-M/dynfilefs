#define _GNU_SOURCE
#include "journal.h"
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// Capture only metadata identities; inspecting storage never creates a mount lock.
static int info_files(const char *path, struct stat files[4])
{
    const char *suffixes[]={"", ".work", ".log", ".rollback"};
    memset(files, 0, 4*sizeof(*files));
    for (size_t i=0; i < 4; i++)
    {
        char name[PATH_MAX];
        int result=storage_name(name, path, suffixes[i]);
        if (result) return result;
        if (lstat(name, &files[i]) && errno!=ENOENT) return -errno;
    }
    return 0;
}

// Decode the latest saved geometry without applying rollback or publishing recovery.
static int inspect_snapshot(const char *path, struct storage_info *info)
{
    struct snapshot snapshot={0}, saved={0};
    int fd=open_suffix(path, "", O_RDONLY);
    if (fd < 0) return fd;
    int result=snapshot_load(fd, &snapshot);
    if (result==-EPROTONOSUPPORT)
    {
        uint64_t header[3];
        char magic[8];
        struct stat file;
        result=fstat(fd, &file) ? -errno : 0;
        if (!result && file.st_size < 4096) result=-EIO;
        if (!result) result=read_at(fd, magic, sizeof(magic), 0);
        if (!result && !memcmp(magic, "DYNFS600", 8)) result=-EPROTONOSUPPORT;
        if (!result) result=read_at(fd, header, sizeof(header), 2048);
        if (!result && le64toh(header[0])!=500) result=-EPROTONOSUPPORT;
        if (!result)
        {
            info->version=500; info->split=le64toh(header[1]); info->size=le64toh(header[2]);
            if (!info->size || info->size > INT64_MAX || info->size%4096) result=-EINVAL;
            if (!info->split || info->split > INT64_MAX || info->split%4096) result=-EINVAL;
        }
        close(fd);
        return result;
    }
    close(fd);
    if (result) return result;
    info->version=DYN_FORMAT;
    fd=open_suffix(path, ".work", O_RDONLY);
    if (fd >= 0)
    {
        result=snapshot_load(fd, &saved);
        close(fd);
        if (!result && saved.part_size!=snapshot.part_size) result=-EIO;
        if (!result && saved.sequence > snapshot.sequence)
        {
            if (saved.size < snapshot.size) result=-EIO;
            else { snapshot_free(&snapshot); snapshot=saved; memset(&saved, 0, sizeof(saved)); }
        }
        snapshot_free(&saved);
    }
    else if (fd!=-ENOENT) result=fd;
    int replayed;
    if (!result) result=journal_replay(path, &snapshot, &replayed);
    if (!result) { info->size=snapshot.size; info->split=snapshot.part_size; }
    snapshot_free(&snapshot);
    return result;
}

// Retry concurrent publication or compaction instead of mixing metadata generations.
int storage_inspect(const char *path, struct storage_info *info)
{
    for (int attempt=0; attempt < 3; attempt++)
    {
        struct stat before[4], after[4];
        int result=info_files(path, before);
        if (result) return result;
        memset(info, 0, sizeof(*info));
        result=inspect_snapshot(path, info);
        int error=info_files(path, after), changed=0;
        if (error) return error;
        for (size_t i=0; i < 4; i++)
        {
            int same_file=before[i].st_dev==after[i].st_dev && before[i].st_ino==after[i].st_ino;
            int same_size=before[i].st_size==after[i].st_size;
            int same_mtime=before[i].st_mtim.tv_sec==after[i].st_mtim.tv_sec && before[i].st_mtim.tv_nsec==after[i].st_mtim.tv_nsec;
            int same_ctime=before[i].st_ctim.tv_sec==after[i].st_ctim.tv_sec && before[i].st_ctim.tv_nsec==after[i].st_ctim.tv_nsec;
            int same_times=same_mtime && same_ctime;
            if (!same_file || !same_size || !same_times) changed=1;
        }
        if (changed) continue;
        if (!result && info->version==DYN_FORMAT) info->rollback=after[3].st_ino!=0;
        return result;
    }
    return -EAGAIN;
}
