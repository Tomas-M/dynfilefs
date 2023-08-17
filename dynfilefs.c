/*
  Author: Tomas M <tomas@slax.org>
  License: GNU GPL

  Dynamic size loop filesystem, provides really big file which is allocated on disk only as needed
  You can then for example make a filesystem on it and mount it using mount -o loop

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
#include <sys/mman.h>
#include <fcntl.h>
#include <pthread.h>
#include <getopt.h>
#include <wait.h>

#define MAX_SPLIT_FILES 9999
#define DATA_BLOCK_SIZE 4096

char *dynfilefs_path = "/virtual.dat";
char *storage_file = "";
char *mount_dir = "";
char *banner = "DynfilefsFS 4.00 (c) 2023 Tomas M <www.slax.org>";
char header[DATA_BLOCK_SIZE] = {};
char empty[DATA_BLOCK_SIZE] = {};

off_t size_MB = 0;
off_t split_size_MB = 0;

off_t format_version=400;
off_t virtual_size = 0;
off_t split_size = 0;
off_t header_size = DATA_BLOCK_SIZE;
off_t offset_block_size = 0;

int max_files = 1;

struct metaStruct
{
   off_t version;
   off_t split_size;
   off_t virtual_size;
};


int debug=0;
int meta_header_offset = DATA_BLOCK_SIZE / 2;

pthread_mutex_t dynfilefs_mutex;
FILE * mainfile;
FILE * files[MAX_SPLIT_FILES] = {0};
char * indexes[MAX_SPLIT_FILES] = {0};
off_t last_block_offsets[MAX_SPLIT_FILES] = {0};


static int with_unlock(int err)
{
   pthread_mutex_unlock(&dynfilefs_mutex);
   return err;
}

static int dynfilefs_fsync(const char *path, int isdatasync, struct fuse_file_info *fi)
{
   for (int ix = 0; ix < max_files; ix++) fflush(files[ix]);
   return 0;
}

static int dynfilefs_flush(const char *path, struct fuse_file_info *fi)
{
   for (int ix = 0; ix < max_files; ix++) fflush(files[ix]);
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
   off_t target = 0;
   off_t seek = 0;

   int ix = offset / split_size;

   seek = header_size + (offset - split_size * ix) / DATA_BLOCK_SIZE * sizeof(offset);
   memcpy(&target, indexes[ix] + seek, sizeof(target));

   return target;
}

// this function is always called when write operation is locked
static off_t create_data_offset(off_t offset)
{
   off_t seek = 0;
   int ix = offset / split_size;

   last_block_offsets[ix] += DATA_BLOCK_SIZE;

   seek = header_size + (offset - split_size * ix) / DATA_BLOCK_SIZE * sizeof(offset);
   memcpy(indexes[ix] + seek, &last_block_offsets[ix], sizeof(last_block_offsets[ix]));

   return last_block_offsets[ix];
}



static int dynfilefs_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
    off_t tot = 0;
    off_t data_offset;
    off_t len = 0;
    off_t rd;
    int ix;

    while (tot < size)
    {
        ix = offset / split_size;

        rd = DATA_BLOCK_SIZE - (offset % DATA_BLOCK_SIZE);
        if (tot + rd > size) rd = size - tot;
        len = rd;

        data_offset = get_data_offset(offset);
        if (data_offset != 0)
        {
           len = pread(fileno(files[ix]), buf, rd, data_offset + (offset % DATA_BLOCK_SIZE));
           if (len == 0) { len = rd; memset(buf, 0, len); }
           if (len < 0) return -errno;
        }
        else
           memset(buf, 0, len);

        tot += len;
        buf += len;
        offset += len;
    }

    return tot;
}


static int dynfilefs_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
    if (offset + size > virtual_size) return -ENOSPC; // do not allow to write beyond file size

    off_t tot = 0;
    off_t data_offset;
    off_t len;
    off_t wr;
    int ix;

    while (tot < size)
    {
       ix = offset / split_size;

       wr = DATA_BLOCK_SIZE - (offset % DATA_BLOCK_SIZE);
       if (tot + wr > size) wr = size - tot;
       len=0;

       pthread_mutex_lock(&dynfilefs_mutex);

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
       }

       pthread_mutex_unlock(&dynfilefs_mutex);

       if (len == 0)
       {
          len = pwrite(fileno(files[ix]), buf, wr, data_offset + (offset % DATA_BLOCK_SIZE));
          if (len <= 0) return -errno;
       }

       tot += len;
       buf += len;
       offset += len;
    }

    return tot;
}

static void dynfilefs_destroy(void *fi)
{
   for (int ix = 0; ix < max_files; ix++)
   {
      fflush(files[ix]);
      fclose(files[ix]);
   }
}

static int dynfilefs_release(const char *path, struct fuse_file_info *fi)
{
   for (int ix = 0; ix < max_files; ix++) fflush(files[ix]);
   return 0;
}

static int dynfilefs_truncate(const char *path, off_t size)
{
   for (int ix = 0; ix < max_files; ix++) fflush(files[ix]);
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
	.destroy	= dynfilefs_destroy,
	.truncate	= dynfilefs_truncate,
};

static void usage(char * cmd)
{
       printf("\n");
       printf("%s\n", banner);
       printf("\n");
       printf("usage: %s -f storage_file -m mount_dir [ -s size_MB ] [ -p split_size_MB ] [ -d ]\n", cmd);
       printf("\n");
       printf("Mount filesystem to [mount_dir], provide a virtual file [mount_dir]/virtual.dat of size [size_MB]\n");
       printf("All changes made to virtual.dat file are stored to [storage_file] file(s)\n");
       printf("\n");
       printf("  -d                       - Do not fork to background, debug mode\n");
       printf("\n");
       printf("  --mountdir [mount_dir]\n");
       printf("  -m [mount_dir]           - Path to a directory where the fileszstem will be mounted\n");
       printf("                           - The directory must be empty, else it will refuse to mount\n");
       printf("\n");
       printf("  --file [storage_file]\n");
       printf("  -f [storage_file]        - Path to a file where all changes will be stored\n");
       printf("                           - If file exists, it will be used\n");
       printf("                           - If file does not exist, it will be created empty\n");
       printf("\n");
       printf("  --size [size_MB]\n");
       printf("  -s [size_MB]             - The virtual.dat file will be size_MB big\n");
       printf("                           - This parameter is ignored if storage file exists,\n");
       printf("                             in that case the previous stored value is reused.\n");
       printf("\n");
       printf("  --split [split_size_MB]\n");
       printf("  -p [split_size_MB ]      - Maximum data size per storage_file. Multiple storage files\n");
       printf("                             will be created if [size_MB] > [split_size_MB].\n");
       printf("                             Beware that actual file size (including index of offsets) may be\n");
       printf("                             bigger than split_size_MB, so use max 4088 on FAT32 to be safe.\n");
       printf("                           - This parameter is ignored if storage file exists,\n");
       printf("                             in that case the previous stored value is reused.\n");
       printf("\n");
       printf("Example usage:\n");
       printf("\n");
       printf("  # %s -f /tmp/changes.dat -s 1024 -m /mnt\n", cmd);
       printf("  # mke2fs -F /mnt/virtual.dat\n");
       printf("  # mount -o loop /mnt/virtual.dat /mnt\n");
       printf("\n");
       printf("The [storage_file] has about 2 MB overhead for each 1GB of data (0.2%%)\n");
       printf("\n");
}

int main(int argc, char *argv[])
{
    int ret=0;

    while (1)
    {
       int option_index = 0;
       static struct option long_options[] = {
           {"file",         required_argument, 0, 'f' },
           {"mountdir",     required_argument, 0, 'm' },
           {"size",         required_argument, 0, 's' },
           {"split",        required_argument, 0, 'p' },
           {"debug",        no_argument,       0, 'd' },
           {0,              0,                 0,  0 }
       };

       int c = getopt_long(argc, argv, "f:m:s:p:d",long_options, &option_index);
       if (c == -1)  break;

       switch (c)
       {
           case 'f':
               storage_file = optarg;
               break;

           case 'm':
               mount_dir = optarg;
               break;

           case 's':
               size_MB = abs(atoi(optarg));
               break;

           case 'p':
               split_size_MB = abs(atoi(optarg));
               break;

           case 'd':
               debug = 1;
               break;
        }
    }

    if (!strcmp(storage_file,"")) { usage(argv[0]); return 1; }

    virtual_size = size_MB * 1024 * 1024;
    split_size = split_size_MB * 1024 * 1024;
    if (split_size <= 0) split_size = virtual_size;

    // open main file when it exists
    mainfile = fopen(storage_file, "r+");
    if (mainfile != NULL)
    {
       struct metaStruct meta = {};

       // check version and other parameters
       fseeko(mainfile, meta_header_offset, SEEK_SET);
       ret = fread(&meta,sizeof(meta),1,mainfile);
       if (ret < 0)
       {
          printf("cannot read header metadata from file %s\n", storage_file);
          return 1;
       }
       if (meta.version != format_version)
       {
          printf("The existing storage file %s is using incompatible data format version %lli. Current version is %lli. This is an error.\n", storage_file, (long long)meta.version, (long long)format_version);
          return 1;
       }

       split_size=meta.split_size;
       if (virtual_size<=meta.virtual_size) virtual_size=meta.virtual_size;

       // if virtual size was changed, write it to main file
       if (meta.virtual_size!=virtual_size)
       {
          meta.virtual_size=virtual_size;
          fseeko(mainfile, meta_header_offset, SEEK_SET);
          ret = fwrite(&meta,sizeof(meta),1,mainfile);
          if (ret < 0)
          {
             printf("cannot update header metadata for new virtual size in file %s\n", storage_file);
             return 1;
          }
       }
    }
    else // file does not exist yet, attempt to create it
    {
       if (virtual_size <= 0) { printf("You must provide virtual file size for new storage file.\n"); return 1; }

       mainfile = fopen(storage_file, "w+");
       if (mainfile == NULL)
       {
          printf("cannot open %s for writing\n", storage_file);
          return 1;
       }

       // write full header (empty)
       fwrite(header,sizeof(header),1,mainfile);

       // write banner to header
       fseeko(mainfile, 0, SEEK_SET);
       fwrite(banner,strlen(banner),1,mainfile);

       // write version to header
       struct metaStruct meta = {version: format_version, split_size: split_size, virtual_size: virtual_size};
       fseeko(mainfile, meta_header_offset, SEEK_SET);
       ret = fwrite(&meta,sizeof(meta),1,mainfile);
       if (ret < 0)
       {
          printf("cannot write to %s\n", storage_file);
          return 1;
       }
    }
    fclose(mainfile);
    utime(storage_file,NULL);

    if (virtual_size > split_size) max_files = virtual_size / split_size + ( virtual_size % split_size > 0 ? 1 : 0);
    offset_block_size = split_size / DATA_BLOCK_SIZE * sizeof(off_t);

    if (max_files > MAX_SPLIT_FILES) { printf("Your settings would result in %i storage files, which is bigger than maximum of %i. Quit\n", max_files, MAX_SPLIT_FILES); return 1; }

    char storage_file_path[4096];

    for (int i=0; i<max_files; i++)
    {
       memset(storage_file_path,0,sizeof(storage_file_path));
       sprintf(storage_file_path, "%s.%i", storage_file, i);

       // open existing changes file
       files[i] = fopen(storage_file_path, "r+");
       if (files[i] != NULL)
       {
          struct metaStruct meta = {};

          // check version and other parameters
          fseeko(files[i], meta_header_offset, SEEK_SET);
          ret = fread(&meta,sizeof(meta),1,files[i]);
          if (ret < 0)
          {
             printf("cannot read header metadata from file %s\n", storage_file_path);
             return 1;
          }
          if (meta.version != format_version)
          {
             printf("The existing storage file %s is using incompatible data format version %lli. Current version is %lli. This is an error.\n", storage_file_path, (long long)meta.version, (long long)format_version);
             return 1;
          }

          if (meta.split_size!=split_size)
          {
             printf("The existing storage file %s was created using split size of %lli. But you requested split size of %lli. This is an error. Use the same split size.\n", storage_file_path, (long long)meta.split_size/1024/1024, (long long)split_size/1024/1024);
             return 1;
          }

          if (meta.virtual_size!=virtual_size)
          {
             meta.virtual_size=virtual_size;
             fseeko(files[i], meta_header_offset, SEEK_SET);
             ret = fwrite(&meta,sizeof(meta),1,files[i]);
             if (ret < 0)
             {
                printf("cannot update header metadata for new virtual size in file %s\n", storage_file_path);
                return 1;
             }
          }

          // calculate new last_block_offsets after index of offsets
          fseeko(files[i], 0, SEEK_END);
          off_t written_data_size = ftello(files[i]) - header_size - offset_block_size;
          if (written_data_size < 0) written_data_size = 0;
          written_data_size += written_data_size % DATA_BLOCK_SIZE; // align to full block
          last_block_offsets[i] = header_size + offset_block_size + written_data_size;
       }
       else // file does not exist yet, attempt to create it
       {
          if (virtual_size <= 0) { printf("You must provide virtual file size for new storage file.\n"); return 1; }

          files[i] = fopen(storage_file_path, "w+");
          if (files[i] == NULL)
          {
             printf("cannot open %s for writing\n", storage_file);
             return 1;
          }

          // write full header (empty)
          fwrite(header,sizeof(header),1,files[i]);

          // write banner to header
          fseeko(files[i], 0, SEEK_SET);
          fwrite(banner,strlen(banner),1,files[i]);

          // write version to header
          struct metaStruct meta = {version: format_version, split_size: split_size, virtual_size: virtual_size};
          fseeko(files[i], meta_header_offset, SEEK_SET);
          ret = fwrite(&meta,sizeof(meta),1,files[i]);
          if (ret < 0)
          {
             printf("cannot write to %s\n", storage_file_path);
             return 1;
          }

          last_block_offsets[i] = header_size + offset_block_size;
          fseeko(files[i],last_block_offsets[i] - 1, SEEK_SET);
          fwrite("\0",1,1,files[i]);
          fflush(files[i]);
       }

       indexes[i] = mmap(NULL, header_size + offset_block_size, PROT_READ|PROT_WRITE, MAP_SHARED, fileno(files[i]), 0);
    }

    // The following line ensures that the process is not killed by systemd
    // on shutdown, it is necessary to keep process running if root filesystem
    // is mounted using dynfilefs. Proper end of the process is umount, not kill.
    argv[0][0] = '@';

    // we're fooling fuse here that we got only one parameter - mountdir
    argv[1] = mount_dir;
    argc=2;

    if (debug) // or maybe two parameters, debug
    {
       argv[argc] = "-d";
       argc++;
    }

    return fuse_main(argc, argv, &dynfilefs_oper, NULL);
}
