#define _GNU_SOURCE
#include "freeze.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

// These stable Linux ioctls also work with musl SDKs that omit kernel UAPI headers.
#define DYN_LOOP_GET_STATUS64 0x4C05
#define DYN_FIFREEZE _IOWR('X', 119, int)
#define DYN_FITHAW _IOWR('X', 120, int)
struct loop_status64
{
    uint64_t lo_device, lo_inode, lo_rdevice, lo_offset, lo_sizelimit;
    uint32_t lo_number, lo_encrypt_type, lo_encrypt_key_size, lo_flags;
    uint8_t lo_file_name[64], lo_crypt_name[64], lo_encrypt_key[32];
    uint64_t lo_init[2];
};

struct freeze_target { int directory, loop; dev_t backing; ino_t inode; };
struct filesystem_freeze { pid_t child; int command, status; };

// Decode the kernel's octal escapes without treating UTF-8 bytes as characters.
static int mount_path(char *path, const char *encoded)
{
    size_t length=0;
    while (*encoded)
    {
        if (length==PATH_MAX-1) return -ENAMETOOLONG;
        if (*encoded=='\\')
        {
            if (encoded[1] < '0' || encoded[1] > '7') return -EIO;
            if (encoded[2] < '0' || encoded[2] > '7') return -EIO;
            if (encoded[3] < '0' || encoded[3] > '7') return -EIO;
            path[length++]=(encoded[1]-'0')*64+(encoded[2]-'0')*8+encoded[3]-'0';
            encoded+=4;
        }
        else path[length++]=*encoded++;
    }
    path[length]=0;
    return 0;
}

// Find an accessible mount of this exact block device in the caller's namespace.
static int mount_directory(dev_t device)
{
    FILE *mounts=fopen("/proc/self/mountinfo", "re");
    if (!mounts) return -errno;
    char *line=NULL;
    size_t capacity=0;
    int result=-EBUSY;
    while (getline(&line, &capacity, mounts) >= 0)
    {
        char *separator=strstr(line, " - "), *next, *field;
        if (!separator) { result=-EIO; break; }
        field=strtok_r(line, " ", &next);
        for (int i=0; field && i < 2; i++) field=strtok_r(NULL, " ", &next);
        unsigned device_major, device_minor;
        if (!field || sscanf(field, "%u:%u", &device_major, &device_minor)!=2)
        { result=-EIO; break; }
        if (makedev(device_major, device_minor)!=device) continue;
        field=strtok_r(NULL, " ", &next);
        if (field) field=strtok_r(NULL, " ", &next);
        if (!field) { result=-EIO; break; }
        char type[16], path[PATH_MAX];
        if (sscanf(separator+3, "%15s", type)!=1) { result=-EIO; break; }
        if (strcmp(type, "ext4") && strcmp(type, "xfs")) { result=-EOPNOTSUPP; continue; }
        result=mount_path(path, field);
        if (result) break;
        int fd=open(path, O_RDONLY|O_DIRECTORY|O_CLOEXEC);
        if (fd < 0) { result=-errno; continue; }
        struct stat st;
        if (fstat(fd, &st)) result=-errno;
        else result=st.st_dev==device ? fd : -ESTALE;
        if (result >= 0) break;
        close(fd);
    }
    if (ferror(mounts) && result < 0) result=-EIO;
    free(line);
    fclose(mounts);
    return result;
}

// A direct loop mount must not hide writable partitions or device-mapper users.
static int direct_loop(const char *name)
{
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "/sys/block/%s/holders", name);
    DIR *directory=opendir(path);
    if (!directory) return -errno;
    struct dirent *entry;
    int result=0;
    for (errno=0; (entry=readdir(directory)); errno=0)
        if (entry->d_name[0]!='.') { result=-EOPNOTSUPP; break; }
    if (!entry && errno) result=-errno;
    closedir(directory);
    if (result) return result;
    snprintf(path, sizeof(path), "/sys/block/%s", name);
    directory=opendir(path);
    if (!directory) return -errno;
    for (errno=0; (entry=readdir(directory)); errno=0)
    {
        if (entry->d_name[0]=='.') continue;
        snprintf(path, sizeof(path), "/sys/block/%s/%s/partition", name, entry->d_name);
        if (!access(path, F_OK)) { result=-EOPNOTSUPP; break; }
        if (errno!=ENOENT && errno!=ENOTDIR) { result=-errno; break; }
    }
    if (!entry && errno) result=-errno;
    closedir(directory);
    return result;
}

// Transfer one complete status code even if a signal interrupts a pipe operation.
static int read_status(int fd)
{
    int result;
    size_t done=0;
    while (done < sizeof(result))
    {
        ssize_t count=read(fd, (char *)&result+done, sizeof(result)-done);
        if (count < 0 && errno==EINTR) continue;
        if (count < 0) return -errno;
        if (!count) return -EIO;
        done+=count;
    }
    return result;
}

// Pipe writes are atomic, but still retry interruptions before reporting failure.
static int write_status(int fd, int result)
{
    ssize_t count;
    do count=write(fd, &result, sizeof(result)); while (count < 0 && errno==EINTR);
    return count==sizeof(result) ? 0 : -EIO;
}

// Do not retain the parent's FUSE connection: its death must fail pending writes.
static void close_unneeded(const struct freeze_target *targets, size_t count, int command, int status, long limit)
{
    for (int fd=0; fd < limit; fd++)
    {
        if (fd==command || fd==status) continue;
        size_t i=0;
        while (i < count && targets[i].directory!=fd && targets[i].loop!=fd) i++;
        if (i==count) close(fd);
    }
}

// Isolate the freeze owner so closing the parent pipe also resumes the filesystem.
static int start_freeze(struct freeze_target *targets, size_t count, struct filesystem_freeze **result)
{
    long limit=sysconf(_SC_OPEN_MAX);
    if (limit < 0) return -EIO;
    struct filesystem_freeze *state=malloc(sizeof(*state));
    if (!state) return -ENOMEM;
    int commands[2], statuses[2];
    if (pipe2(commands, O_CLOEXEC)) { free(state); return -errno; }
    if (pipe2(statuses, O_CLOEXEC))
    { int error=-errno; close(commands[0]); close(commands[1]); free(state); return error; }
    pid_t child=fork();
    if (!child)
    {
        signal(SIGPIPE, SIG_IGN); signal(SIGINT, SIG_IGN); signal(SIGTERM, SIG_IGN); signal(SIGHUP, SIG_IGN);
        close_unneeded(targets, count, commands[0], statuses[1], limit);
        int error=0;
        size_t frozen=0;
        for (; frozen < count; frozen++)
        {
            struct loop_status64 info;
            if (ioctl(targets[frozen].loop, DYN_LOOP_GET_STATUS64, &info)) { error=-errno; break; }
            if (info.lo_device!=targets[frozen].backing || info.lo_inode!=targets[frozen].inode)
            { error=-ESTALE; break; }
            if (info.lo_offset || info.lo_sizelimit) { error=-EOPNOTSUPP; break; }
            if (ioctl(targets[frozen].directory, DYN_FIFREEZE, 0)) { error=-errno; break; }
        }
        if (!error && !write_status(statuses[1], 0))
        {
            char command;
            while (read(commands[0], &command, 1) < 0 && errno==EINTR) {}
        }
        for (size_t i=frozen; i-- > 0;)
            if (ioctl(targets[i].directory, DYN_FITHAW, 0) && !error) error=-errno;
        write_status(statuses[1], error);
        _exit(error ? 1 : 0);
    }
    int error=child < 0 ? -errno : 0;
    close(commands[0]); close(statuses[1]);
    if (error)
    { close(commands[1]); close(statuses[0]); free(state); return error; }
    state->child=child; state->command=commands[1]; state->status=statuses[0];
    error=read_status(state->status);
    if (error) { filesystem_thaw(state); return error; }
    *result=state;
    return 0;
}

// Match by backing inode/device; names alone are insufficient after renames or binds.
int filesystem_freeze(int directory, const char *disk_path, struct filesystem_freeze **result)
{
    *result=NULL;
    struct stat backing;
    if (fstatat(directory, disk_path, &backing, 0)) return -errno;
    DIR *loops=opendir("/sys/block");
    if (!loops) return -errno;
    struct freeze_target *targets=NULL;
    size_t count=0;
    struct dirent *entry;
    int error=0;
    for (errno=0; (entry=readdir(loops)); errno=0)
    {
        if (strncmp(entry->d_name, "loop", 4) || !entry->d_name[4]) continue;
        if (strspn(entry->d_name+4, "0123456789")!=strlen(entry->d_name+4)) continue;
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "/sys/block/%s/loop/backing_file", entry->d_name);
        if (access(path, F_OK))
        {
            if (errno==ENOENT) continue;
            error=-errno; break;
        }
        snprintf(path, sizeof(path), "/dev/%s", entry->d_name);
        int loop=open(path, O_RDONLY|O_CLOEXEC);
        if (loop < 0) { error=-errno; break; }
        struct loop_status64 info;
        if (ioctl(loop, DYN_LOOP_GET_STATUS64, &info))
        {
            int saved=errno;
            close(loop);
            if (saved==ENXIO) continue;
            error=-saved; break;
        }
        if (info.lo_device!=backing.st_dev || info.lo_inode!=backing.st_ino) { close(loop); continue; }
        if (info.lo_offset || info.lo_sizelimit) error=-EOPNOTSUPP;
        if (!error) error=direct_loop(entry->d_name);
        struct stat device;
        if (!error && fstat(loop, &device)) error=-errno;
        int mount=error ? error : mount_directory(device.st_rdev);
        if (mount < 0) { error=mount; close(loop); break; }
        struct freeze_target *grown=realloc(targets, (count+1)*sizeof(*targets));
        if (!grown) { error=-ENOMEM; close(mount); close(loop); break; }
        targets=grown;
        targets[count++]=(struct freeze_target){mount, loop, backing.st_dev, backing.st_ino};
    }
    if (!entry && errno) error=-errno;
    closedir(loops);
    if (!error && count) error=start_freeze(targets, count, result);
    for (size_t i=0; i < count; i++) { close(targets[i].directory); close(targets[i].loop); }
    free(targets);
    return error;
}

// Closing the command pipe requests thaw, then wait until the helper confirms it.
int filesystem_thaw(struct filesystem_freeze *state)
{
    if (!state) return 0;
    close(state->command);
    int error=read_status(state->status), status;
    close(state->status);
    pid_t waited;
    do waited=waitpid(state->child, &status, 0); while (waited < 0 && errno==EINTR);
    if (waited < 0 && !error) error=-errno;
    if (waited >= 0 && (!WIFEXITED(status) || WEXITSTATUS(status)) && !error) error=-EIO;
    free(state);
    return error;
}
