/*
  Author: Tomas M <tomas@slax.org>
  License: GNU GPL

  Dynamic size loop filesystem, provides really big file which is allocated on disk only as needed
  You can then make a filesystem on it and mount it using -o loop,sync

  This program can be distributed under the terms of the GNU GPL.
  See the file COPYING.
*/

#define _ATFILE_SOURCE 1
#define _GNU_SOURCE 1
#define FUSE_USE_VERSION 26

#include <fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <wait.h>

static const char *dynfilefs_path = "/loop.fs";
static const char *save_path = "changes.dat";
static const char *header = "DynfilefsFS 2.20 (c) 2012 Tomas M <www.slax.org>";
off_t virtual_size = 0;
off_t first_index = 0;
off_t zero = 0;

static pthread_mutex_t dynfilefs_mutex;

#define DATA_BLOCK_SIZE 4096
#define NUM_INDEXED_BLOCKS 16384

FILE * fp;
static const char empty[DATA_BLOCK_SIZE];

#include <dyfslib.c>

static int with_unlock(int err)
{
   pthread_mutex_unlock(&dynfilefs_mutex);
   return err;
}

static int dynfilefs_fsync(const char *path, int isdatasync,
		     struct fuse_file_info *fi)
{
	(void) path;
	(void) isdatasync;
	(void) fi;
	fflush(fp);
	return 0;
}


static int dynfilefs_getattr(const char *path, struct stat *stbuf)
{
	int res = 0;

	memset(stbuf, 0, sizeof(struct stat));
	if (strcmp(path, "/") == 0) {
		stbuf->st_mode = S_IFDIR | 0755;
		stbuf->st_nlink = 2;
	} else if (strcmp(path, dynfilefs_path) == 0) {
		stbuf->st_mode = S_IFREG | 0444;
		stbuf->st_nlink = 1;
		stbuf->st_size = virtual_size;
	} else
		res = -ENOENT;

	return res;
}

static int dynfilefs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
			 off_t offset, struct fuse_file_info *fi)
{
	(void) offset;
	(void) fi;

	if (strcmp(path, "/") != 0)
		return -ENOENT;

	filler(buf, ".", NULL, 0);
	filler(buf, "..", NULL, 0);
	filler(buf, dynfilefs_path + 1, NULL, 0);

	return 0;
}

static int dynfilefs_open(const char *path, struct fuse_file_info *fi)
{
	if (strcmp(path, dynfilefs_path) != 0)
		return -ENOENT;

        (void) fi;
	return 0;
}

static int dynfilefs_read(const char *path, char *buf, size_t size, off_t offset,
		      struct fuse_file_info *fi)
{
    if (strcmp(path, dynfilefs_path) != 0) return -ENOENT;

    off_t tot = 0;
    off_t data_offset;
    off_t len = 0;
    off_t rd;
    (void) fi;

    pthread_mutex_lock(&dynfilefs_mutex);

    while (tot < size)
    {
        data_offset = get_data_offset(offset);
        if (data_offset != 0)
        {
           rd = DATA_BLOCK_SIZE - (offset % DATA_BLOCK_SIZE);
           if (tot + rd > size) rd = size - tot;
           fseeko(fp, data_offset + (offset % DATA_BLOCK_SIZE), SEEK_SET);
           len = fread(buf, 1, rd, fp);
        }

        if (len < 0) return with_unlock(-errno);

        if (len == 0 || data_offset == 0)
        {
           len = DATA_BLOCK_SIZE - (offset % DATA_BLOCK_SIZE);
           memset(buf, 0, len);
        }
        tot += len;
        buf += len;
        offset += len;
    }

    pthread_mutex_unlock(&dynfilefs_mutex);
    return tot;
}


static int dynfilefs_write(const char *path, const char *buf, size_t size,
		     off_t offset, struct fuse_file_info *fi)
{
    if(strcmp(path, dynfilefs_path) != 0) return -ENOENT;

    off_t tot = 0;
    off_t data_offset;
    off_t len;
    off_t wr;
    (void) fi;

    pthread_mutex_lock(&dynfilefs_mutex);

    while (tot < size)
    {
       data_offset = get_data_offset(offset);
       wr = DATA_BLOCK_SIZE - (offset % DATA_BLOCK_SIZE);
       if (tot + wr > size) wr = size - tot;

       // skip writing empty blocks if not already exist
       if (!memcmp(&empty, buf, wr) && data_offset == 0)
       {
          len = wr;
       }
       else // write block
       {
          if (data_offset == 0) data_offset = create_data_offset(offset);
          if (data_offset == 0) return with_unlock(-ENOSPC); // write error, not enough free space
          fseeko(fp, data_offset + (offset % DATA_BLOCK_SIZE), SEEK_SET);
          len = fwrite(buf, 1, wr, fp);
          if (len < 0) return with_unlock(-errno);
       }
       tot += len;
       buf += len;
       offset += len;
    }

    pthread_mutex_unlock(&dynfilefs_mutex);
    return tot;
}


static int dynfilefs_flush(const char *path, struct fuse_file_info *fi)
{
	(void) path;
	(void) fi;
	fflush(fp);
	return 0;
}

static struct fuse_operations dynfilefs_oper = {
	.getattr	= dynfilefs_getattr,
	.readdir	= dynfilefs_readdir,
	.open		= dynfilefs_open,
	.read		= dynfilefs_read,
	.write		= dynfilefs_write,
	.fsync		= dynfilefs_fsync,
	.flush		= dynfilefs_flush,
};

static void usage(char * cmd)
{
       printf("usage: %s [storage_file] [sizeMB] [mountpoint]\n", cmd);
       printf("\n");
       printf("Mount filesystem to [mountpoint], provide a virtual file ./loop.fs\n");
       printf("of size [sizeMB] so you can make a filesystem on it and mount it.\n");
       printf("Store all changes made to loop.fs to [storage_file]\n");
       printf("\n");
       printf("  [storage_file] - path to a file where all changes will be stored.\n");
       printf("  [sizeMB]       - loop.fs will be sizeMB * 1024 * 1024 - 1 bytes long\n");
       printf("\n");
       printf("Example usage:\n");
       printf("\n");
       printf("  # %s /tmp/changes.dat 1024 /mnt\n", cmd);
       printf("  # mke2fs -F /mnt/loop.fs\n");
       printf("  # mount -o loop,sync /mnt/loop.fs /mnt\n");
       printf("\n");
       printf("Be aware that the [storage_file] has about 0.4%% overhead, thus if you\n");
       printf("put [sizeMB] data to loop.fs then the [storage_file] will be little bigger\n");
       printf("than the [sizeMB] specified. This is important if you store changes on VFAT,\n");
       printf("since VFAT has 4GB file size limit, thus you can't store full 4096 MB of data\n");
       printf("due to the overhead. Each 64MB of data needs 256k+8 bytes index.\n");
}

int main(int argc, char *argv[])
{
    int ret;

    if (argc < 4)
    {
       usage(argv[0]);
       return 12;
    }

    save_path = argv[1];
    off_t sizeMB = atoi(argv[2]);
    virtual_size = sizeMB * 1024 * 1024 - 1;

    if (sizeMB <= 0)
    {
       usage(argv[0]);
       return 13;
    }

    // we're fooling fuse here that we got only one parameter - mountdir
    argv[1] = argv[3];
//    argv[2] = "-d";  // uncomment for debug
    argc=2;
//    argc=3; // uncomment for debug

    // open save data file
    fp = fopen(save_path, "r+");
    if (fp == NULL)
    {
       // create empty dataset
       fp = fopen(save_path, "w+");
       if (fp == NULL)
       {
          printf("cannot open %s for writing\n", save_path);
          return 14;
       }

       ret = fwrite(header,strlen(header),1,fp);
       if (ret < 0)
       {
          printf("cannot write to %s\n", save_path);
          return 15;
       }
       fseeko(fp, strlen(header) + NUM_INDEXED_BLOCKS*sizeof(zero)*2, SEEK_SET);
       ret = fwrite(&zero,sizeof(zero),1,fp);
    }

    if (fp == NULL)
    {
       printf("cannot open %s for writing\n", save_path);
       return 16;
    }

    fseeko(fp, 0, SEEK_SET);

    // first index is always right after the header. Get the position
    first_index = strlen(header);

    // empty block is needed for comparison. Blocks full of null bytes are not stored
    memset(&empty, 0, sizeof(empty));

    return fuse_main(argc, argv, &dynfilefs_oper, NULL);
}
