/*
  Author: Tomas M <tomas@slax.org>
  License: GNU GPL

  A dynamically allocated virtual disk with one mount's rollback point.
*/

#define _GNU_SOURCE 1
#define FUSE_USE_VERSION 31

#include <fuse.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "storage.h"

static struct storage *disk;
static const char *disk_path="/virtual.dat";
static const char *rollback_path="/rollback";

// Expose the fixed-size virtual disk and its session rollback control.
static int dynfilefs_getattr(const char *path, struct stat *st, struct fuse_file_info *fi)
{
    memset(st, 0, sizeof(*st));
    if (!strcmp(path, "/"))
    {
        st->st_mode=S_IFDIR|0755;
        st->st_nlink=2;
    }
    else if (!strcmp(path, disk_path))
    {
        st->st_mode=S_IFREG|0444;
        st->st_nlink=1;
        st->st_size=storage_size(disk);
    }
    else if (!strcmp(path, rollback_path))
    {
        st->st_mode=S_IFREG|0600;
        st->st_nlink=1;
        st->st_size=2;
        st->st_uid=getuid();
        st->st_gid=getgid();
    }
    else return -ENOENT;
    return 0;
}

// List the virtual disk and its writable rollback flag.
static int dynfilefs_readdir(const char *path, void *buffer, fuse_fill_dir_t fill, off_t offset, struct fuse_file_info *fi, enum fuse_readdir_flags flags)
{
    if (strcmp(path, "/")) return -ENOENT;
    fill(buffer, ".", NULL, 0, 0);
    fill(buffer, "..", NULL, 0, 0);
    fill(buffer, disk_path+1, NULL, 0, 0);
    fill(buffer, rollback_path+1, NULL, 0, 0);
    return 0;
}

// Bypass the page cache so every control read sees the current flag.
static int dynfilefs_open(const char *path, struct fuse_file_info *fi)
{
    if (!strcmp(path, disk_path)) return 0;
    if (strcmp(path, rollback_path)) return -ENOENT;
    fi->direct_io=1;
    return 0;
}

// Negotiate large requests and descriptor-backed direct reads.
static void *dynfilefs_init(struct fuse_conn_info *connection, struct fuse_config *config)
{
    connection->max_read=1024*1024;
    if (connection->max_write > 1024*1024) connection->max_write=1024*1024;
    connection->want|=connection->capable&(FUSE_CAP_SPLICE_WRITE|FUSE_CAP_SPLICE_MOVE);
    return NULL;
}

// Return the live flag as text, or let storage handle sparse disk reads.
static int dynfilefs_read(const char *path, char *buffer, size_t size, off_t offset, struct fuse_file_info *fi)
{
    if (!strcmp(path, disk_path)) return storage_read(disk, buffer, size, offset);
    if (strcmp(path, rollback_path)) return -ENOENT;
    if (offset < 0) return -EINVAL;
    if (offset >= 2) return 0;
    char value[2]={'0'+storage_get_rollback(disk), '\n'};
    if (size > 2-(size_t)offset) size=2-(size_t)offset;
    memcpy(buffer, value+offset, size);
    return size;
}

// Persist a complete 0/1 control write before acknowledging it.
static int dynfilefs_write(const char *path, const char *buffer, size_t size, off_t offset, struct fuse_file_info *fi)
{
    if (!strcmp(path, disk_path)) return storage_write(disk, buffer, size, offset);
    if (strcmp(path, rollback_path)) return -ENOENT;
    if (!size) return 0;
    if (offset || (size != 1 && size != 2)) return -EINVAL;
    if (size == 2 && buffer[1] != '\n') return -EINVAL;
    if (buffer[0] != '0' && buffer[0] != '1') return -EINVAL;
    int error=storage_set_rollback(disk, buffer[0]=='1');
    return error ? error : (int)size;
}

// Splice only complete contiguous direct reads; holes and partial tails use memory.
static int dynfilefs_read_buf(const char *path, struct fuse_bufvec **result, size_t size, off_t offset, struct fuse_file_info *fi)
{
    int control=!strcmp(path, rollback_path);
    if (!control && strcmp(path, disk_path)) return -ENOENT;
    if (offset < 0) return -EINVAL;
    uint64_t length=control ? 2 : storage_size(disk);
    if ((uint64_t)offset >= length) size=0;
    else if (size > length-(uint64_t)offset) size=length-(uint64_t)offset;
    struct fuse_bufvec *buffer=malloc(sizeof(*buffer));
    if (!buffer) return -ENOMEM;
    *buffer=FUSE_BUFVEC_INIT(size);
    if (size > 8192 && (fi->flags&O_DIRECT))
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

// Synchronization retains the rollback point until the mount finishes cleanly.
static int dynfilefs_fsync(const char *path, int datasync, struct fuse_file_info *fi)
{
    if (!strcmp(path, disk_path)) return storage_sync(disk);
    return strcmp(path, rollback_path) ? -ENOENT : 0;
}

// Keep sizes fixed; shell redirection must not change the flag before its write.
static int dynfilefs_truncate(const char *path, off_t size, struct fuse_file_info *fi)
{
    if (!strcmp(path, disk_path)) return 0;
    if (strcmp(path, rollback_path)) return -ENOENT;
    return size ? -EINVAL : 0;
}

// Preserve the virtual file's existing no-op permission-change behavior.
static int dynfilefs_chmod(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    return strcmp(path, disk_path) && strcmp(path, rollback_path) ? -ENOENT : 0;
}

// Ownership belongs to the mount rather than individual storage blocks.
static int dynfilefs_chown(const char *path, uid_t uid, gid_t gid, struct fuse_file_info *fi)
{
    return strcmp(path, disk_path) && strcmp(path, rollback_path) ? -ENOENT : 0;
}

static const struct fuse_operations operations={
    .init=dynfilefs_init,
    .getattr=dynfilefs_getattr,
    .readdir=dynfilefs_readdir,
    .open=dynfilefs_open,
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
    printf("DynFileFS 6.00 (storage format %d)\n\n", DYN_FORMAT);
    printf("usage: %s -f storage_file -m mount_dir [-s size_MB] [-p split_MB] [-d] [--norollback]\n", program);
    printf("       %s -o size=size_MB,split=split_MB storage_file mount_dir\n", program);
    printf("       %s -i -f storage_file\n", program);
    printf("\n");
    printf("  -f, --file PATH       Storage metadata file; data parts use .0, .1, ...\n");
    printf("  -m, --mountdir PATH   Empty mount directory containing virtual.dat and rollback.\n");
    printf("  -s, --size MiB        Virtual size; +MiB grows existing storage by a delta.\n");
    printf("                       Smaller sizes are ignored; a new disk needs a size.\n");
    printf("  -p, --split MiB       Maximum physical size of each part, including indexes.\n");
    printf("                       Defaults to 4088 MiB and is retained for existing storage.\n");
    printf("  -d                   Stay in the foreground and enable FUSE debugging.\n");
    printf("  -i, --info           Print saved storage metadata as key:value lines; no mount or changes.\n");
    printf("  --norollback         Skip a pending rollback for this mount; keep its flag enabled.\n");
    printf("                       Also available as -o norollback.\n");
    printf("\nFormat-500 storage is converted automatically on its first mount.\n");
    printf("Parts and 4 KiB index pages are created only as needed.\n");
    printf("Write 1 to mount_dir/rollback to discard this session at the next mount.\n");
    printf("Write 0 to cancel the request; reading rollback returns the current flag.\n");
    printf("Clean unmount saves the current state; pending rollback waits for a normal mount.\n");
    printf("After interruption, the last fsync checkpoint is recovered unless rollback was requested.\n");
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

// Keep byte counts exact and report only complete MiB for shell arithmetic.
static void print_size(const char *name, uint64_t bytes)
{
    printf("%s:%" PRIu64 "\n%sMiB:%" PRIu64 "\n", name, bytes, name, bytes/1048576);
}

// Parse mount and fstab options.
int main(int argc, char **argv)
{
    const char *file="", *mount="";
    uint64_t size=0, increase=0, split=0;
    int debug=0, norollback=0, info=0, mount_options=0, option;
    static const struct option options[]={
        {"options", required_argument, NULL, 'o'},
        {"file", required_argument, NULL, 'f'},
        {"mountdir", required_argument, NULL, 'm'},
        {"size", required_argument, NULL, 's'},
        {"split", required_argument, NULL, 'p'},
        {"debug", no_argument, NULL, 'd'},
        {"info", no_argument, NULL, 'i'},
        {"norollback", no_argument, NULL, 'n'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };
    while ((option=getopt_long(argc, argv, "f:m:s:p:o:dih", options, NULL)) != -1)
    {
        if (strchr("mspodn", option)) mount_options=1;
        switch (option)
        {
            case 'f': file=optarg; break;
            case 'm': mount=optarg; break;
            case 's': size=parse_size(optarg); increase=optarg[0]=='+' ? size : 0; break;
            case 'p': split=parse_size(optarg); break;
            case 'd': debug=1; break;
            case 'i': info=1; break;
            case 'n': norollback=1; break;
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
                    else if (!strcmp(value, "norollback")) norollback=1;
                }
                break;
            }
            default: fprintf(stderr, "Try '%s --help' for usage.\n", argv[0]); return 1;
        }
    }
    if (!*file && optind < argc) file=argv[optind++];
    if (info)
    {
        int invalid_path=!*file || *mount || optind!=argc;
        if (invalid_path || mount_options)
        {
            fprintf(stderr, "Usage: %s -i -f storage_file (without mount options)\n", argv[0]);
            return 1;
        }
        struct storage_info saved;
        int error=storage_inspect(file, &saved);
        if (error) { fprintf(stderr, "Cannot inspect storage: %s\n", strerror(-error)); return 1; }
        printf("version:%" PRIu64 "\n", saved.version);
        print_size("size", saved.size);
        print_size("split", saved.split);
        printf("blockSize:%d\nrollback:%d\n", DYN_BLOCK_SIZE, saved.rollback);
        return fflush(stdout)==EOF;
    }
    if (!*mount && optind < argc) mount=argv[optind++];
    if (!*file || !*mount || optind != argc) { usage(argv[0]); return 1; }
    int error=storage_open(&disk, file, size, increase, split, norollback);
    if (error)
    {
        fprintf(stderr, "Cannot open storage: %s\n", strerror(-error));
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
    storage_close(disk);
    return result;
}
