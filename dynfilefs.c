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

static const char *dynfilefs_path = "/virtual.dat";
static const char *save_path = "changes.dat";
static const char *header = "DynfilefsFS 3.00 (c) 2023 Tomas M <www.slax.org>";
off_t virtual_size = 0;
off_t header_size = 0;
off_t last_block_offset = 0;
off_t zero = 0;

static pthread_mutex_t dynfilefs_mutex;

#define DATA_BLOCK_SIZE 4096

FILE * fp;
static char empty[DATA_BLOCK_SIZE];

static int with_unlock(int err)
{
   pthread_mutex_unlock(&dynfilefs_mutex);
   return err;
}

static int dynfilefs_fsync(const char *path, int isdatasync, struct fuse_file_info *fi)
{
   fflush(fp);
   return 0;
}

static int dynfilefs_flush(const char *path, struct fuse_file_info *fi)
{
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

static int dynfilefs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi)
{
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

	return 0;
}



// discover real position of data for offset
//
static off_t get_data_offset(off_t offset)
{
   int ret;
   off_t target = 0;
   off_t seek = 0;

   seek = header_size + sizeof(virtual_size) + offset / DATA_BLOCK_SIZE * sizeof(offset);
   fseeko(fp, seek, SEEK_SET);
   ret = fread(&target, sizeof(target), 1, fp);

   if (ret < 0) return 0;

   return target;
}


static off_t create_data_offset(off_t offset)
{
   int ret;
   last_block_offset += DATA_BLOCK_SIZE;

   offset = header_size + sizeof(virtual_size) + offset / DATA_BLOCK_SIZE * sizeof(offset);

   fseeko(fp, offset, SEEK_SET);
   ret = fwrite(&last_block_offset, sizeof(last_block_offset), 1, fp);
   if (ret < 0) return 0;

   return last_block_offset;
}



static int dynfilefs_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{

    if (strcmp(path, dynfilefs_path) != 0) return -ENOENT;
    off_t tot = 0;
    off_t data_offset;
    off_t len = 0;
    off_t rd;

    pthread_mutex_lock(&dynfilefs_mutex);

    while (tot < size)
    {
        rd = DATA_BLOCK_SIZE - (offset % DATA_BLOCK_SIZE);
        if (tot + rd > size) rd = size - tot;
        len = rd;

        data_offset = get_data_offset(offset);
        if (data_offset != 0)
        {
           fseeko(fp, data_offset + (offset % DATA_BLOCK_SIZE), SEEK_SET);
           len = fread(buf, 1, rd, fp);
           if (len == 0) { len = rd; memset(buf, 0, len); }
           if (len < 0) return with_unlock(-errno);
        }
        else
           memset(buf, 0, len);

        tot += len;
        buf += len;
        offset += len;
    }

    pthread_mutex_unlock(&dynfilefs_mutex);
    return tot;
}


static int dynfilefs_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
    if(strcmp(path, dynfilefs_path) != 0) return -ENOENT;
    if (offset + size > virtual_size) return with_unlock(-ENOSPC); // do not allow to write beyond file size

    off_t tot = 0;
    off_t data_offset;
    off_t len;
    off_t wr;

    pthread_mutex_lock(&dynfilefs_mutex);

    while (tot < size)
    {
       wr = DATA_BLOCK_SIZE - (offset % DATA_BLOCK_SIZE);
       if (tot + wr > size) wr = size - tot;

       data_offset = get_data_offset(offset);

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
          if (len <= 0) return with_unlock(-errno);
       }
       tot += len;
       buf += len;
       offset += len;
    }

    pthread_mutex_unlock(&dynfilefs_mutex);
    return tot;
}

static void dynfilefs_destroy(void *fi)
{
   fflush(fp);
   fclose(fp);
}

static int dynfilefs_release(const char *path, struct fuse_file_info *fi)
{
   fflush(fp);
   return 0;
}

static int dynfilefs_ioctl(const char *path, int cmd, void *arg, struct fuse_file_info *fi, unsigned int flags, void *data)
{
   fflush(fp);
   return 0;
}

static int dynfilefs_truncate(const char *path, off_t size)
{
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
	.release	= dynfilefs_release,
	.ioctl		= dynfilefs_ioctl,
	.destroy	= dynfilefs_destroy,
	.truncate	= dynfilefs_truncate,
};

static void usage(char * cmd)
{
       printf("\n");
       printf("%s\n", header);
       printf("\n");
       printf("usage: %s [storage_file] [sizeMB] [mountpoint]\n", cmd);
       printf("\n");
       printf("Mount filesystem to [mountpoint], provide a virtual file [mountpoint]/virtual.dat of size [sizeMB]\n");
       printf("All changes made to virtual.dat file are stored to [storage_file]\n");
       printf("\n");
       printf("  [storage_file] - path to a file where all changes will be stored.\n");
       printf("  [sizeMB]       - virtual.dat will be sizeMB * 1024 * 1024 bytes long\n");
       printf("                 - this parameter is ignored if [storage_file] already exists\n");
       printf("                   since in that case, the size is read from the storage_file\n");
       printf("\n");
       printf("Example usage:\n");
       printf("\n");
       printf("  # %s /tmp/changes.dat 1024 /mnt\n", cmd);
       printf("  # mke2fs -F /mnt/virtual.dat\n");
       printf("  # mount -o loop,sync /mnt/virtual.dat /mnt\n");
       printf("\n");
       printf("Be aware that the [storage_file] has about 2 MB overhead for each 1GB of data,\n");
       printf("thus the size of [storage_file] will be little bigger than [sizeMB] specified.\n");
       printf("This is important if you plan to save [storage_file] on VFAT,\n");
       printf("since VFAT has 4GB file size limit, so you need to use sizeMB <= 4087\n");
       printf("on VFAT due to the overhead.\n");
}

int main(int argc, char *argv[])
{
    int ret;

    if (argc < 4)
    {
       usage(argv[0]);
       return 11;
    }

    off_t sizeMB = atoi(argv[2]);
    if (sizeMB <= 0)
    {
       usage(argv[0]);
       return 12;
    }

    save_path = argv[1];

    header_size = strlen(header);
    virtual_size = sizeMB * 1024 * 1024;

    // The following line ensures that the process is not killed by systemd
    // on shutdown, it is necessary to keep process running if root filesystem
    // is mounted using dynfilefs. Proper end of the process is umount, not kill.
    argv[0][0] = '@';

    // we're fooling fuse here that we got only one parameter - mountdir
    argv[1] = argv[3];
    //argv[2] = "-d";  // uncomment for debug
    argc=2;
    //argc=3; // uncomment for debug

    // open existing changes file
    fp = fopen(save_path, "r+");
    if (fp != NULL)
    {
       //check first 14 bytes of header if file content is in compatible DynFileFS format
       fseeko(fp, 0, SEEK_SET);
       char saved_header[4096] = "";
       ret = fread(saved_header, 14, 1, fp);
       if (ret < 0)
       {
          printf("cannot read header of from file %s\n", save_path);
          return 13;
       }
       if (strncmp(saved_header, header, 14) !=0 )
       {
          printf("The existing file %s is not in proper format. Accepted format is only: %.14s\n", save_path, header);
          return 14;
       }

       fseeko(fp, header_size, SEEK_SET);
       ret = fread(&virtual_size, sizeof(virtual_size), 1, fp);
       if (ret < 0)
       {
          printf("cannot read size of virtual file from the file %s\n", save_path);
          return 15;
       }

       // calculate new last_block_offset
       off_t metadata_size = header_size + sizeof(virtual_size) + virtual_size / DATA_BLOCK_SIZE * sizeof(off_t);
       fseeko(fp, 0, SEEK_END);
       off_t written_data_size = ftello(fp) - metadata_size;
       if (written_data_size < 0) written_data_size = 0;
       off_t written_blocks = written_data_size / DATA_BLOCK_SIZE;
       last_block_offset = metadata_size + written_blocks * DATA_BLOCK_SIZE;
    }
    else // file does not exist yet or cannot be opened, attempt to create it
    {
       fp = fopen(save_path, "w+");
       if (fp == NULL)
       {
          printf("cannot open %s for writing\n", save_path);
          return 16;
       }

       // write header
       ret = fwrite(header,strlen(header),1,fp);
       if (ret < 0)
       {
          printf("cannot write header to %s\n", save_path);
          return 16;
       }

       // write size
       ret = fwrite(&virtual_size,sizeof(virtual_size),1,fp);
       if (ret < 0)
       {
          printf("cannot write to %s\n", save_path);
          return 17;
       }

       last_block_offset = header_size + sizeof(virtual_size) + virtual_size / DATA_BLOCK_SIZE * sizeof(off_t);
    }

    fseeko(fp, 0, SEEK_SET);

    // empty block is needed for comparison. Blocks full of null bytes are not stored
    memset(&empty, 0, sizeof(empty));

    return fuse_main(argc, argv, &dynfilefs_oper, NULL);
}
