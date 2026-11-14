/*
  Author: Tomas M <tomas@slax.org>
  License: GNU GPL

  A dynamically allocated virtual disk with automatic session snapshots and named history.
*/

#define _GNU_SOURCE 1
#define FUSE_USE_VERSION 31

#include <fuse.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "storage.h"
#include "format.h"
#include "freeze.h"
#include "report.h"

static struct storage *disk;
static const char *disk_path="/virtual.dat";
static const char *snapshot_path="/snapshot";
static const char *snapshots_path="/snapshots";
static const char *stats_path="/stats";
static const char *history_path="/snapshots.json";
static char virtual_path[PATH_MAX];
static int virtual_directory=-1;
static pthread_mutex_t snapshot_commands=PTHREAD_MUTEX_INITIALIZER;
struct name_list { char *bytes; size_t length; };

// Recognize one immutable image without interpreting its international name.
static int snapshot_name(const char *path, char *name)
{
    if (strncmp(path, "/snapshots/", 11)) return 0;
    const char *file=path+11;
    size_t length=strlen(file);
    if (length <= 4 || length > NAME_MAX) return 0;
    if (strchr(file, '/') || strcmp(file+length-4, ".dat")) return 0;
    memcpy(name, file, length-4);
    name[length-4]=0;
    return 1;
}

// Text handles keep one complete report while other requests change live state.
static int text_file(const char *path)
{
    return !strcmp(path, snapshot_path) || !strcmp(path, stats_path) || !strcmp(path, history_path);
}

// Share one JSON schema between offline inspection and mounted read-only reports.
static int text_content(const char *path, char **bytes)
{
    if (!strcmp(path, snapshot_path)) return storage_snapshot_list(disk, bytes);
    if (!strcmp(path, history_path)) return storage_snapshot_report(disk, 1, bytes);
    struct storage_info info;
    int result=storage_get_info(disk, &info);
    return result ? result : storage_info_report(&info, 1, bytes);
}

// Freeze each listing for one open handle so large reads and readdir offsets agree.
static int open_text(const char *path, struct fuse_file_info *fi)
{
    struct name_list *list=calloc(1, sizeof(*list));
    if (!list) return -ENOMEM;
    int result=text_content(path, &list->bytes);
    if (result < 0) { free(list); return result; }
    list->length=result;
    fi->fh=(uintptr_t)list;
    return 0;
}

// Expose the working disk, live controls and immutable saved images.
static int dynfilefs_getattr(const char *path, struct stat *st, struct fuse_file_info *fi)
{
    memset(st, 0, sizeof(*st));
    char name[NAME_MAX+1];
    if (!strcmp(path, "/") || !strcmp(path, snapshots_path))
    {
        st->st_mode=S_IFDIR|0755;
        if (!strcmp(path, snapshots_path)) st->st_mode=S_IFDIR|0555;
        st->st_nlink=2;
    }
    else if (!strcmp(path, disk_path))
    {
        st->st_mode=S_IFREG|0444;
        st->st_nlink=1;
        st->st_size=storage_size(disk);
    }
    else if (text_file(path))
    {
        st->st_mode=S_IFREG|0400;
        if (!strcmp(path, snapshot_path)) st->st_mode=S_IFREG|0600;
        st->st_nlink=1;
        if (fi && fi->fh) st->st_size=((struct name_list *)(uintptr_t)fi->fh)->length;
        else
        {
            char *bytes;
            int result=text_content(path, &bytes);
            if (result < 0) return result;
            st->st_size=result;
            free(bytes);
        }
        st->st_uid=getuid();
        st->st_gid=getgid();
    }
    else if (snapshot_name(path, name))
    {
        uint64_t size;
        int result=storage_snapshot_stat(disk, name, &size);
        if (result) return result;
        st->st_mode=S_IFREG|0444;
        st->st_nlink=1;
        st->st_size=size;
    }
    else return -ENOENT;
    return 0;
}

// Pin a directory listing without holding snapshot images open.
static int dynfilefs_opendir(const char *path, struct fuse_file_info *fi)
{
    if (!strcmp(path, "/")) return 0;
    return strcmp(path, snapshots_path) ? -ENOENT : open_text(snapshot_path, fi);
}

// Use resumable offsets for directories containing many snapshot names.
static int dynfilefs_readdir(const char *path, void *buffer, fuse_fill_dir_t fill, off_t offset, struct fuse_file_info *fi, enum fuse_readdir_flags flags)
{
    if (offset < 0) return -EINVAL;
    if (strcmp(path, "/") && strcmp(path, snapshots_path)) return -ENOENT;
    const char *entries[]={".", "..", disk_path+1, snapshot_path+1, snapshots_path+1, stats_path+1, history_path+1};
    size_t count=!strcmp(path, "/") ? sizeof(entries)/sizeof(*entries) : 2;
    for (size_t i=0; i < count; i++)
        if ((off_t)i >= offset && fill(buffer, entries[i], NULL, i+1, 0)) return 0;
    if (!strcmp(path, "/")) return 0;
    struct name_list *list=(struct name_list *)(uintptr_t)fi->fh;
    off_t position=2;
    for (char *start=list->bytes; start && *start;)
    {
        char *end=strchr(start, '\n'), name[NAME_MAX+1];
        size_t length=end ? (size_t)(end-start) : strlen(start);
        if (length+4 > NAME_MAX) return -EIO;
        memcpy(name, start, length);
        memcpy(name+length, ".dat", 5);
        if (position >= offset && fill(buffer, name, NULL, position+1, 0)) return 0;
        position++;
        start=end ? end+1 : NULL;
    }
    return 0;
}

// Pin saved mappings until release and reject every writable snapshot open.
static int dynfilefs_open(const char *path, struct fuse_file_info *fi)
{
    if (!strcmp(path, disk_path)) return 0;
    if (text_file(path))
    {
        int writable=(fi->flags&O_ACCMODE)!=O_RDONLY || (fi->flags&O_TRUNC);
        if (strcmp(path, snapshot_path) && writable) return -EROFS;
        fi->direct_io=1;
        return (fi->flags&O_ACCMODE)==O_WRONLY ? 0 : open_text(path, fi);
    }
    char name[NAME_MAX+1];
    if (snapshot_name(path, name))
    {
        if ((fi->flags&O_ACCMODE)!=O_RDONLY || (fi->flags&O_TRUNC)) return -EROFS;
        struct storage_snapshot *view;
        int result=storage_snapshot_open(disk, name, &view);
        if (result) return result;
        fi->fh=(uintptr_t)view;
        fi->direct_io=1;
        return 0;
    }
    return -ENOENT;
}

// Drop directory/control buffers or the reference protecting a saved image.
static int dynfilefs_release(const char *path, struct fuse_file_info *fi)
{
    if (!fi->fh) return 0;
    if (text_file(path) || !strcmp(path, snapshots_path))
    {
        struct name_list *list=(struct name_list *)(uintptr_t)fi->fh;
        free(list->bytes);
        free(list);
    }
    else storage_snapshot_close(disk, (struct storage_snapshot *)(uintptr_t)fi->fh);
    fi->fh=0;
    return 0;
}

// Negotiate large requests and descriptor-backed direct reads.
static void *dynfilefs_init(struct fuse_conn_info *connection, struct fuse_config *config)
{
    connection->max_read=1024*1024;
    if (connection->max_write > 1024*1024) connection->max_write=1024*1024;
    connection->want|=connection->capable&(FUSE_CAP_SPLICE_WRITE|FUSE_CAP_SPLICE_MOVE);
    config->entry_timeout=0;
    config->attr_timeout=0;
    return NULL;
}

// Route immutable reads through their pinned mapping and controls through text.
static int dynfilefs_read(const char *path, char *buffer, size_t size, off_t offset, struct fuse_file_info *fi)
{
    if (!strcmp(path, disk_path)) return storage_read(disk, buffer, size, offset);
    if (offset < 0) return -EINVAL;
    if (text_file(path))
    {
        struct name_list *list=(struct name_list *)(uintptr_t)fi->fh;
        if ((uint64_t)offset >= list->length) return 0;
        if (size > list->length-offset) size=list->length-offset;
        memcpy(buffer, list->bytes+offset, size);
        return size;
    }
    char name[NAME_MAX+1];
    if (snapshot_name(path, name)) return storage_snapshot_read(disk, (struct storage_snapshot *)(uintptr_t)fi->fh, buffer, size, offset);
    return -ENOENT;
}

// Persist complete control commands while leaving FUSE workers available to freeze I/O.
static int dynfilefs_write(const char *path, const char *buffer, size_t size, off_t offset, struct fuse_file_info *fi)
{
    if (!strcmp(path, disk_path)) return storage_write(disk, buffer, size, offset);
    if (!strcmp(path, snapshot_path))
    {
        if (!size) return 0;
        if (offset || size > NAME_MAX+8 || memchr(buffer, 0, size)) return -EINVAL;
        char command[NAME_MAX+9];
        memcpy(command, buffer, size);
        command[size]=0;
        if (command[size-1]=='\n') command[size-1]=0;
        int create=!strncmp(command, "create ", 7);
        if (!create && strncmp(command, "delete ", 7)) return -EINVAL;
        if (pthread_mutex_trylock(&snapshot_commands)) return -EBUSY;
        int result;
        if (create)
        {
            struct filesystem_freeze *state=NULL;
            result=filesystem_freeze(virtual_directory, virtual_path, &state);
            if (!result)
            {
                result=storage_snapshot_create(disk, command+7);
                int thaw=filesystem_thaw(state);
                if (thaw) fprintf(stderr, "Cannot thaw snapshot filesystem: %s\n", strerror(-thaw));
                if (!result) result=thaw;
            }
        }
        else result=storage_snapshot_delete(disk, command+7);
        pthread_mutex_unlock(&snapshot_commands);
        return result ? result : (int)size;
    }
    char name[NAME_MAX+1];
    if (text_file(path) || snapshot_name(path, name)) return -EROFS;
    return -ENOENT;
}

// Splice only complete contiguous direct reads; holes and partial tails use memory.
static int dynfilefs_read_buf(const char *path, struct fuse_bufvec **result, size_t size, off_t offset, struct fuse_file_info *fi)
{
    if (offset < 0) return -EINVAL;
    int working=!strcmp(path, disk_path);
    uint64_t length;
    char name[NAME_MAX+1];
    if (working) length=storage_size(disk);
    else if (text_file(path)) length=((struct name_list *)(uintptr_t)fi->fh)->length;
    else if (snapshot_name(path, name)) length=storage_snapshot_size((struct storage_snapshot *)(uintptr_t)fi->fh);
    else return -ENOENT;
    if ((uint64_t)offset >= length) size=0;
    else if (size > length-(uint64_t)offset) size=length-(uint64_t)offset;
    struct fuse_bufvec *buffer=malloc(sizeof(*buffer));
    if (!buffer) return -ENOMEM;
    *buffer=FUSE_BUFVEC_INIT(size);
    if (working && size > 8192 && (fi->flags&O_DIRECT))
    {
        int fd;
        off_t position;
        int complete=storage_read_extent(disk, offset, size, &fd, &position);
        if (complete < 0) { free(buffer); return complete; }
        if (complete)
        {
            buffer->buf[0].flags=FUSE_BUF_IS_FD|FUSE_BUF_FD_SEEK|FUSE_BUF_FD_RETRY;
            buffer->buf[0].fd=fd;
            buffer->buf[0].pos=position;
        }
    }
    if (size && !(buffer->buf[0].flags&FUSE_BUF_IS_FD))
    {
        buffer->buf[0].mem=malloc(size);
        if (!buffer->buf[0].mem) { free(buffer); return -ENOMEM; }
        int count=dynfilefs_read(path, buffer->buf[0].mem, size, offset, fi);
        if (count < 0) { free(buffer->buf[0].mem); free(buffer); return count; }
        buffer->buf[0].size=count;
    }
    *result=buffer;
    return 0;
}

// Synchronization preserves the latest recoverable working state.
static int dynfilefs_fsync(const char *path, int datasync, struct fuse_file_info *fi)
{
    if (!strcmp(path, disk_path)) return storage_sync(disk);
    char name[NAME_MAX+1];
    return text_file(path) || snapshot_name(path, name) ? 0 : -ENOENT;
}

// Keep sizes fixed; shell redirection must not change a control before its write.
static int dynfilefs_truncate(const char *path, off_t size, struct fuse_file_info *fi)
{
    if (!strcmp(path, disk_path)) return 0;
    char name[NAME_MAX+1];
    if (!strcmp(path, snapshot_path)) return size ? -EINVAL : 0;
    return text_file(path) || snapshot_name(path, name) ? -EROFS : -ENOENT;
}

// Preserve the virtual file's existing no-op permission-change behavior.
static int dynfilefs_chmod(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    char name[NAME_MAX+1];
    if (!strcmp(path, disk_path) || !strcmp(path, snapshot_path)) return 0;
    return text_file(path) || snapshot_name(path, name) ? -EROFS : -ENOENT;
}

// Ownership belongs to the mount rather than individual storage blocks.
static int dynfilefs_chown(const char *path, uid_t uid, gid_t gid, struct fuse_file_info *fi)
{
    char name[NAME_MAX+1];
    if (!strcmp(path, disk_path) || !strcmp(path, snapshot_path)) return 0;
    return text_file(path) || snapshot_name(path, name) ? -EROFS : -ENOENT;
}

static const struct fuse_operations operations={
    .init=dynfilefs_init,
    .getattr=dynfilefs_getattr,
    .opendir=dynfilefs_opendir,
    .readdir=dynfilefs_readdir,
    .releasedir=dynfilefs_release,
    .open=dynfilefs_open,
    .release=dynfilefs_release,
    .read=dynfilefs_read,
    .write=dynfilefs_write,
    .read_buf=dynfilefs_read_buf,
    .fsync=dynfilefs_fsync,
    .truncate=dynfilefs_truncate,
    .chmod=dynfilefs_chmod,
    .chown=dynfilefs_chown,
};

// Keep command help aligned with the physical split limit and session semantics.
static void usage(const char *program)
{
    printf("DynFileFS 6.02 (storage format %d)\n\n", DYN_FORMAT);
    printf("usage: %s -f storage_file -m mount_dir [-s size_MiB] [-p split_MiB] [-d] [--restore NAME]\n", program);
    printf("       %s -o size=size_MiB,split=split_MiB storage_file mount_dir\n", program);
    printf("       %s -f storage_file --info|--snapshots [--json]\n", program);
    printf("\n");
    printf("  -f, --file PATH       Storage metadata file; data parts use .0, .1, ...\n");
    printf("  -m, --mountdir PATH   Empty mount directory for virtual.dat and snapshot controls.\n");
    printf("  -s, --size MiB        Virtual size; +MiB grows existing storage by a delta.\n");
    printf("                       Smaller sizes are ignored; a new disk needs a size.\n");
    printf("  -p, --split MiB       Maximum physical part size, including indexes; default 4088 MiB.\n");
    printf("  -d                   Stay in the foreground and enable FUSE debugging.\n");
    printf("  -i, --info           Inspect geometry and reusableBytes without mounting or changes.\n");
    printf("  --snapshots          Show the saved history tree, including deleted ancestors.\n");
    printf("  --json               Return --info or --snapshots as JSON.\n");
    printf("  --restore NAME       Start from a saved point; also available as -o restore=NAME.\n");
    printf("\nFormat-500 storage converts automatically on its first mount.\n");
    printf("Each session starts at a saved point; a changed clean unmount creates a new one.\n");
    printf("The latest ten automatic points are retained; manually named points stay until deleted.\n");
    printf("After interruption, the last successful fsync is recovered.\n");
    printf("Read mount_dir/snapshot to list saved names; write 'create NAME' or 'delete NAME'.\n");
    printf("Creation freezes a detected direct loop-mounted ext4/XFS filesystem automatically.\n");
    printf("Saved images are read-only files at mount_dir/snapshots/NAME.dat.\n");
    printf("Read stats and snapshots.json for live JSON capacity and history reports.\n");
    printf("An open saved image cannot be deleted; reusable blocks are consumed before growing parts.\n");
}

// Parse MiB without allowing overflow of the signed file-offset range.
static uint64_t parse_size(const char *value)
{
    char *end;
    errno=0;
    long long size=strtoll(value, &end, 10);
    int invalid=errno || end==value || *end;
    if (invalid || size < 0 || size > LLONG_MAX/1024/1024)
    {
        fprintf(stderr, "Invalid size in MiB: %s\n", value);
        exit(1);
    }
    return (uint64_t)size*1024*1024;
}

// Parse mount and fstab options.
int main(int argc, char **argv)
{
    const char *file="", *mount="", *restore=NULL;
    uint64_t size=0, increase=0, split=0;
    int debug=0, info=0, snapshots=0, json=0, mount_options=0, option;
    static const struct option options[]={
        {"options", required_argument, NULL, 'o'},
        {"file", required_argument, NULL, 'f'},
        {"mountdir", required_argument, NULL, 'm'},
        {"size", required_argument, NULL, 's'},
        {"split", required_argument, NULL, 'p'},
        {"debug", no_argument, NULL, 'd'},
        {"info", no_argument, NULL, 'i'},
        {"snapshots", no_argument, NULL, 'S'},
        {"json", no_argument, NULL, 'j'},
        {"restore", required_argument, NULL, 'r'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };
    while ((option=getopt_long(argc, argv, "f:m:s:p:o:r:dih", options, NULL)) != -1)
    {
        if (strchr("mspodr", option)) mount_options=1;
        switch (option)
        {
            case 'f': file=optarg; break;
            case 'm': mount=optarg; break;
            case 's': size=parse_size(optarg); increase=optarg[0]=='+' ? size : 0; break;
            case 'p': split=parse_size(optarg); break;
            case 'd': debug=1; break;
            case 'i': info=1; break;
            case 'S': snapshots=1; break;
            case 'j': json=1; break;
            case 'r': restore=optarg; break;
            case 'h': usage(argv[0]); return 0;
            case 'o':
            {
                char *next;
                for (char *value=strtok_r(optarg, ",", &next); value; value=strtok_r(NULL, ",", &next))
                {
                    if (!strncmp(value, "size=", 5))
                    {
                        size=parse_size(value+5);
                        increase=value[5]=='+' ? size : 0;
                    }
                    else if (!strncmp(value, "split=", 6)) split=parse_size(value+6);
                    else if (!strncmp(value, "restore=", 8)) restore=value+8;
                }
                break;
            }
            default: fprintf(stderr, "Try '%s --help' for usage.\n", argv[0]); return 1;
        }
    }
    if (!*file && optind < argc) file=argv[optind++];
    if (info || snapshots)
    {
        int invalid_path=!*file || *mount || optind!=argc;
        if (invalid_path || mount_options || (info && snapshots))
        {
            fprintf(stderr, "Usage: %s -f storage_file --info|--snapshots [--json] (without mount options)\n", argv[0]);
            return 1;
        }
        char *report=NULL;
        int result;
        if (info)
        {
            struct storage_info saved;
            result=storage_inspect(file, &saved);
            if (!result) result=storage_info_report(&saved, json, &report);
        }
        else result=storage_snapshot_inspect(file, json, &report);
        if (result < 0)
        {
            if (result==-EBUSY) fprintf(stderr, "Storage is mounted; read stats or snapshots.json in its mount directory.\n");
            else fprintf(stderr, "Cannot inspect storage: %s\n", strerror(-result));
            return 1;
        }
        int failed=fwrite(report, 1, result, stdout)!=(size_t)result;
        free(report);
        return fflush(stdout)==EOF || failed;
    }
    if (json) { fprintf(stderr, "--json requires --info or --snapshots.\n"); return 1; }
    if (!*mount && optind < argc) mount=argv[optind++];
    if (!*file || !*mount || optind != argc) { usage(argv[0]); return 1; }
    if (restore && !*restore) { fprintf(stderr, "A named snapshot is required for --restore.\n"); return 1; }
    char *directory=realpath(mount, NULL);
    if (!directory) { fprintf(stderr, "Cannot resolve mount directory: %s\n", strerror(errno)); return 1; }
    // Pin the parent, not the FUSE root itself: an owned mount handle would prevent unmount.
    char *name=NULL;
    int error=storage_path(directory, &virtual_directory, &name);
    free(directory);
    if (error) { fprintf(stderr, "Cannot open mount parent: %s\n", strerror(-error)); return 1; }
    int length=snprintf(virtual_path, sizeof(virtual_path), "%s%s", name, disk_path);
    free(name);
    if (length >= (int)sizeof(virtual_path)) { close(virtual_directory); fprintf(stderr, "Mount directory is too long.\n"); return 1; }
    error=storage_open(&disk, file, size, increase, split, restore);
    if (error)
    {
        fprintf(stderr, "Cannot open storage: %s\n", strerror(-error));
        close(virtual_directory);
        return 1;
    }

    // Retain systemd's shutdown exemption until the mounted filesystem is unmounted.
    argv[0][0]='@';
    char *fuse_args[]={argv[0], (char *)mount, "-o", "max_read=1048576", debug ? "-d" : NULL, NULL};
    int result=fuse_main(debug ? 5 : 4, fuse_args, &operations, NULL);
    // Signal and loop-error exits also destroy FUSE, but must not commit the session.
    if (!result)
    {
        int finish_error=storage_finish(disk);
        if (finish_error) fprintf(stderr, "Cannot finish storage: %s\n", strerror(-finish_error));
        result=!!finish_error;
    }
    close(virtual_directory);
    storage_close(disk);
    return result;
}
