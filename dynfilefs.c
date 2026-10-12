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
#define FUSE_USE_VERSION 31

#include <fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <utime.h>
#include <string.h>
#include <errno.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <pthread.h>
#include <getopt.h>
#include <wait.h>
#include <limits.h>

#define MAX_SPLIT_FILES 9999
#define HEADER_SIZE 4096
#define DATA_BLOCK_SIZE 4096

char *dynfilefs_path = "/virtual.dat";
char *storage_file = "";
char *mount_dir = "";
char *banner = "DynfilefsFS 5.01 (c) 2026 Tomas M <www.slax.org>";
char header[HEADER_SIZE] = {};
char empty[DATA_BLOCK_SIZE] = {};

off_t size_MB = 0;
off_t split_size_MB = 0;
off_t increase_size_MB = 0;

off_t format_version=500;
off_t virtual_size = 0;
off_t split_size = 0;
off_t header_size = HEADER_SIZE;
off_t offset_block_size = 0;

int max_files = 1;

struct metaStruct
{
   off_t version;
   off_t split_size;
   off_t virtual_size;
};


int debug=0;
int meta_header_offset = HEADER_SIZE / 2;

pthread_mutex_t storage_locks[MAX_SPLIT_FILES];
FILE * mainfile;
FILE * files[MAX_SPLIT_FILES] = {0};
char * indexes[MAX_SPLIT_FILES] = {0};
off_t last_block_offsets[MAX_SPLIT_FILES] = {0};
int storage_dirty[MAX_SPLIT_FILES] = {0};


// Release the split's lock while preserving an I/O error.
static int with_unlock(int ix, int err)
{
   pthread_mutex_unlock(&storage_locks[ix]);
   return err;
}

static int dynfilefs_fsync(const char *path, int isdatasync, struct fuse_file_info *fi)
{
   for (int ix = 0; ix < max_files; ix++)
   {
      pthread_mutex_lock(&storage_locks[ix]);
      if (storage_dirty[ix])
      {
         // Linux fsync persists both file data and its MAP_SHARED index pages.
         if (fsync(fileno(files[ix]))) return with_unlock(ix, -errno);
         storage_dirty[ix]=0;
      }
      pthread_mutex_unlock(&storage_locks[ix]);
   }
   return 0;
}

static int dynfilefs_flush(const char *path, struct fuse_file_info *fi)
{
   return 0;
}


static int dynfilefs_getattr(const char *path, struct stat *stbuf, struct fuse_file_info *fi)
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

static int dynfilefs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi, enum fuse_readdir_flags flags)
{
	if (strcmp(path, "/") != 0)
		return -ENOENT;

	filler(buf, ".", NULL, 0, 0);
	filler(buf, "..", NULL, 0, 0);
	filler(buf, dynfilefs_path + 1, NULL, 0, 0);

	return 0;
}


static int dynfilefs_open(const char *path, struct fuse_file_info *fi)
{
	if (strcmp(path, dynfilefs_path) != 0)
		return -ENOENT;

	return 0;
}


// Negotiate large writes and descriptor-backed read replies.
static void *dynfilefs_init(struct fuse_conn_info *conn, struct fuse_config *config)
{
    conn->max_read=1024*1024;
    if (conn->max_write > 1024*1024) conn->max_write=1024*1024;
    conn->want|=conn->capable&(FUSE_CAP_SPLICE_WRITE|FUSE_CAP_SPLICE_MOVE);
    return NULL;
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

// Combine blocks only while their physical positions (or holes) are contiguous.
static size_t get_data_size(off_t offset, size_t size, off_t target)
{
    int ix=offset/split_size;
    off_t position=offset-split_size*ix;
    if (size > split_size-position) size=split_size-position;
    size_t len=DATA_BLOCK_SIZE-offset%DATA_BLOCK_SIZE;
    if (len > size) len=size;
    char *entry=indexes[ix]+header_size+position/DATA_BLOCK_SIZE*sizeof(off_t);
    while (len < size)
    {
        off_t next;
        entry+=sizeof(off_t);
        memcpy(&next, entry, sizeof(next));
        if (!target && next) break;
        if (target && next!=target+offset%DATA_BLOCK_SIZE+len) break;
        size_t part=size-len;
        if (part > DATA_BLOCK_SIZE) part=DATA_BLOCK_SIZE;
        len+=part;
    }
    return len;
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
    if (offset < 0) return -EINVAL;
    if (offset >= virtual_size) return 0;
    if (size > virtual_size-offset) size=virtual_size-offset;

    off_t tot = 0;
    off_t data_offset;
    off_t len = 0;
    off_t rd;
    int ix;

    while (tot < size)
    {
        ix = offset / split_size;

        pthread_mutex_lock(&storage_locks[ix]);
        data_offset = get_data_offset(offset);
        rd=get_data_size(offset, size-tot, data_offset);
        pthread_mutex_unlock(&storage_locks[ix]);
        len=rd;
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
    if (offset < 0) return -EINVAL;
    if (offset > virtual_size || size > virtual_size-offset) return -ENOSPC;

    off_t tot = 0;
    off_t data_offset;
    off_t len;
    off_t wr;
    int ix;

    while (tot < size)
    {
       ix = offset / split_size;

       // Independent split files can perform I/O concurrently.
       pthread_mutex_lock(&storage_locks[ix]);
       data_offset = get_data_offset(offset);
       int new_block=data_offset==0;
       wr=get_data_size(offset, size-tot, data_offset);
       size_t first=DATA_BLOCK_SIZE-offset%DATA_BLOCK_SIZE;
       if (first > wr) first=wr;
       int skip=new_block && !memcmp(empty, buf, first);

       // Keep zero blocks sparse when grouping newly allocated blocks.
       if (new_block)
           for (size_t run=first; run < wr;)
           {
               size_t part=wr-run;
               if (part > DATA_BLOCK_SIZE) part=DATA_BLOCK_SIZE;
               int empty_block=!memcmp(empty, buf+run, part);
               if (empty_block!=skip) { wr=run; break; }
               run+=part;
           }
       if (skip)
       {
          len = wr;
       }
       else // write block
       {
          if (new_block) data_offset=last_block_offsets[ix]+DATA_BLOCK_SIZE;
          storage_dirty[ix]=1;
          len = pwrite(fileno(files[ix]), buf, wr, data_offset + (offset % DATA_BLOCK_SIZE));
          if (len <= 0) return with_unlock(ix, len < 0 ? -errno : -EIO);
          // Publish only blocks touched by a successful (possibly short) write.
          if (new_block)
              for (off_t block=0; block < (offset%DATA_BLOCK_SIZE+len+DATA_BLOCK_SIZE-1)/DATA_BLOCK_SIZE; block++)
                  create_data_offset(offset+block*DATA_BLOCK_SIZE);
       }

       pthread_mutex_unlock(&storage_locks[ix]);

       tot += len;
       buf += len;
       offset += len;
    }

    return tot;
}

// Splice large direct reads; buffered loop reads, holes and partial tails use pread.
static int dynfilefs_read_buf(const char *path, struct fuse_bufvec **bufp, size_t size, off_t offset, struct fuse_file_info *fi)
{
    if (offset < 0) return -EINVAL;
    if (offset >= virtual_size) size=0;
    else if (size > virtual_size-offset) size=virtual_size-offset;
    struct fuse_bufvec *buffer=malloc(sizeof(*buffer));
    if (!buffer) return -ENOMEM;
    *buffer=FUSE_BUFVEC_INIT(size);
    // Keep the splice cutoff independent of the storage allocation size.
    if (size > 8192 && (fi->flags&O_DIRECT))
    {
        int ix=offset/split_size;
        pthread_mutex_lock(&storage_locks[ix]);
        off_t target=get_data_offset(offset);
        off_t position=target+offset%DATA_BLOCK_SIZE;
        int complete=target && get_data_size(offset, size, target)==size;
        if (complete && position+size > last_block_offsets[ix])
        {
            struct stat st;
            if (fstat(fileno(files[ix]), &st))
            {
                free(buffer);
                return with_unlock(ix, -errno);
            }
            complete=position+size <= st.st_size;
        }
        if (complete)
        {
            buffer->buf[0].flags=FUSE_BUF_IS_FD|FUSE_BUF_FD_SEEK|FUSE_BUF_FD_RETRY;
            buffer->buf[0].fd=fileno(files[ix]);
            buffer->buf[0].pos=position;
        }
        pthread_mutex_unlock(&storage_locks[ix]);
    }
    if (size && !(buffer->buf[0].flags&FUSE_BUF_IS_FD))
    {
        buffer->buf[0].mem=malloc(size);
        if (!buffer->buf[0].mem) { free(buffer); return -ENOMEM; }
        int result=dynfilefs_read(path, buffer->buf[0].mem, size, offset, fi);
        if (result < 0) { free(buffer->buf[0].mem); free(buffer); return result; }
        buffer->buf[0].size=result;
    }
    *bufp=buffer;
    return 0;
}

static void dynfilefs_destroy(void *fi)
{
   int error=dynfilefs_fsync(dynfilefs_path, 0, NULL);
   if (error) fprintf(stderr, "Cannot synchronize storage on unmount: %s\n", strerror(-error));
   for (int ix = 0; ix < max_files; ix++)
   {
      fclose(files[ix]);
      pthread_mutex_destroy(&storage_locks[ix]);
   }
}

static int dynfilefs_release(const char *path, struct fuse_file_info *fi)
{
   return 0;
}

static int dynfilefs_truncate(const char *path, off_t size, struct fuse_file_info *fi)
{
   return 0;
}

static int dynfilefs_chmod(const char *path, mode_t mode, struct fuse_file_info *fi)
{
   return 0;
}

static int dynfilefs_chown(const char *path, uid_t uid, gid_t gid, struct fuse_file_info *fi)
{
   return 0;
}


static struct fuse_operations dynfilefs_oper = {
	.init           = dynfilefs_init,
	.getattr	= dynfilefs_getattr,
	.readdir	= dynfilefs_readdir,
	.open		= dynfilefs_open,
	.read		= dynfilefs_read,
	.write		= dynfilefs_write,
	.read_buf       = dynfilefs_read_buf,
	.fsync		= dynfilefs_fsync,
	.flush		= dynfilefs_flush,
	.release	= dynfilefs_release,
	.destroy	= dynfilefs_destroy,
	.truncate	= dynfilefs_truncate,
	.chmod		= dynfilefs_chmod,
	.chown		= dynfilefs_chown,
};

static void usage(char * cmd)
{
       printf("\n");
       printf("%s\n", banner);
       printf("\n");
       printf("usage: %s -f storage_file -m mount_dir [ -s size_MB ] [ -p split_size_MB ] [ -d ]\n", cmd);
       printf("       %s -o[size=size_MB][split=split_size_MB] storage_file mount_dir\n", cmd);
       printf("\n");
       printf("This command mounts a virtual filesystem to [mount_dir], creating a virtual file [mount_dir]/virtual.dat\n");
       printf("with a specified size in MB [size_MB]. All modifications to this virtual.dat file are then stored to disk,\n");
       printf("to files with [storge_file] base name.\n");
       printf("\n");
       printf("Metadata related to the virtual.dat file is saved to the [storage_file] itself,\n");
       printf("while actual data modifications are stored in separate files with the same base name,\n");
       printf("each having incremented extension (e.g., .0, .1, .2), depending on the specified split_size.\n");
       printf("\n");
       printf("The following parameters can be provided:\n\n");
       printf("\n");
       printf("  -d                       - Debug mode; do not fork to background\n");
       printf("\n");
       printf("  --file [storage_file]\n");
       printf("  [storage_file]\n");
       printf("  -f [storage_file]        - Path to the file where changes to the virtual file will be stored.\n");
       printf("                           - The storage file is created with the provided name to store metadata,\n");
       printf("                             and then additional storage files are created with the same base name\n");
       printf("                             with extension suffixes such as .0, .1, .2, etc.\n");
       printf("                           - If the storage exists, it will be reused.\n");
       printf("\n");
       printf("  --mountdir [mount_dir]\n");
       printf("  [mount_dir]\n");
       printf("  -m [mount_dir]           - Specifies the directory where the filesystem will be mounted.\n");
       printf("                           - The directory must be empty, or the mount operation will be refused.\n");
       printf("\n");
       printf("  --size [size_MB]\n");
       printf("  -o size=[size_MB]\n");
       printf("  -s [size_MB]             - Sets the size of the virtual.dat file in MB.\n");
       printf("                           - If storage file exists, you can specify bigger size_MB than before,\n");
       printf("                             in that case the size of virtual file will be enlarged.\n");
       printf("                           - If the specified size_MB is smaller than before, it will be ignored\n");
       printf("                             and the previous stored value of size_MB will be reused.\n");
       printf("                           - If the size is specified as +size_MB (note the plus sign prefix),\n");
       printf("                             then the virtual file will grow by size_MB if storage_file exists.\n");
       printf("\n");
       printf("  --split [split_size_MB]\n");
       printf("  -o split=[split_size_MB]\n");
       printf("  -p [split_size_MB ]      - Sets the maximum data size per storage file. Multiple files\n");
       printf("                             will be created if [size_MB] > [split_size_MB].\n");
       printf("                             Beware that actual file size (including internal indexes) may be\n");
       printf("                             bigger than split_size_MB, so use max 4088 on FAT32 to be safe,\n");
       printf("                             because FAT32 does not support individual files bigger than 4GB.\n");
       printf("                           - This parameter is ignored if storage file exists,\n");
       printf("                             in that case the previous stored value is reused.\n");
       printf("\n");
       printf("Example usage:\n");
       printf("\n");
       printf("  # %s -f /tmp/changes.dat -s 1024 -m /mnt\n", cmd);
       printf("  # mke2fs -F /mnt/virtual.dat\n");
       printf("  # mount -o loop /mnt/virtual.dat /mnt\n");
       printf("\n");
       printf("Each 4 KiB data block uses an 8-byte index entry (about 0.2%% overhead).\n");
       printf("\n");
}

// Reject invalid sizes and values that overflow when converted to bytes.
static off_t parse_size_MB(const char *value)
{
    char *end;
    errno=0;
    long long size=strtoll(value, &end, 10);
    int invalid=errno || end==value || *end;
    if (invalid || size < 0 || size > LLONG_MAX/1024/1024)
    {
        fprintf(stderr, "Invalid size in MB: %s\n", value);
        exit(1);
    }
    return size;
}

// The last size option determines whether to grow by a delta or use an absolute size.
static void set_size_MB(const char *optarg)
{
    size_MB=parse_size_MB(optarg);
    increase_size_MB=optarg[0]=='+' ? size_MB : 0;
}

// Split sizes use the same numeric limits as virtual sizes.
static void set_split_size_MB(const char *optarg)
{
    split_size_MB=parse_size_MB(optarg);
}

// Validate storage geometry before updating metadata or mapping an index.
static int validate_storage_sizes(void)
{
    if (virtual_size <= 0 || split_size <= 0 || split_size%DATA_BLOCK_SIZE)
    {
        fprintf(stderr, "Invalid virtual or split size.\n");
        return 0;
    }
    if ((virtual_size-1)/split_size >= MAX_SPLIT_FILES)
    {
        fprintf(stderr, "Storage requires more than %i split files.\n", MAX_SPLIT_FILES);
        return 0;
    }
    max_files=(virtual_size-1)/split_size+1;
    offset_block_size=split_size/DATA_BLOCK_SIZE*sizeof(off_t);
    if (offset_block_size > SIZE_MAX-header_size) return 0;
    return 1;
}

// Flush metadata while errors can still be reported to the caller.
static int write_metadata(FILE *file)
{
    struct metaStruct meta={format_version, split_size, virtual_size};
    if (fseeko(file, meta_header_offset, SEEK_SET)) return 0;
    if (fwrite(&meta, sizeof(meta), 1, file) != 1) return 0;
    return fflush(file)==0;
}

int main(int argc, char *argv[])
{
    while (1)
    {
       int option_index = 0;
       static struct option long_options[] = {
           {"options",      required_argument, 0, 'o'},
           {"file",         required_argument, 0, 'f' },
           {"mountdir",     required_argument, 0, 'm' },
           {"size",         required_argument, 0, 's' },
           {"split",        required_argument, 0, 'p' },
           {"debug",        no_argument,       0, 'd' },
           {0,              0,                 0,  0 }
       };

       int c = getopt_long(argc, argv, "f:o:m:s:p:d",long_options, &option_index);
       if (c == -1) break;

       switch (c)
       {
           case 'f':
               storage_file = optarg;
               break;

           case 'm':
               mount_dir = optarg;
               break;
           case 'o':
           {
               // Parse each mount option without skipping its first value character.
               char *next;
               for (char *option=strtok_r(optarg, ",", &next); option; option=strtok_r(NULL, ",", &next))
               {
                   if (!strncmp(option, "size=", 5)) set_size_MB(option+5);
                   else if (!strncmp(option, "split=", 6)) set_split_size_MB(option+6);
               }
               break;
           }
           case 's':
               set_size_MB(optarg);
               break;

           case 'p':
               set_split_size_MB(optarg);
               break;

           case 'd':
               debug = 1;
               break;
           default:
               usage(argv[0]);
               return 1;
        }
    }

    if (!*storage_file && optind < argc) storage_file=argv[optind++];
    if (!*mount_dir && optind < argc) mount_dir=argv[optind++];
    if (!*storage_file || !*mount_dir || optind < argc) { usage(argv[0]); return 1; }

    virtual_size = size_MB * 1024 * 1024;
    split_size = split_size_MB * 1024 * 1024;
    if (split_size <= 0) split_size = virtual_size;
    memcpy(header, banner, strlen(banner));

    // open main file when it exists
    mainfile = fopen(storage_file, "r+");
    if (mainfile != NULL)
    {
       struct metaStruct meta = {};

       // check version and other parameters
       if (fseeko(mainfile, meta_header_offset, SEEK_SET) || fread(&meta,sizeof(meta),1,mainfile) != 1)
       {
          printf("cannot read header metadata from file %s\n", storage_file);
          return 1;
       }
       if (meta.version != format_version)
       {
          printf("The existing storage file %s is using incompatible data format version %lli. Current version is %lli. This is an error.\n", storage_file, (long long)meta.version, (long long)format_version);
          return 1;
       }

       if (meta.virtual_size <= 0 || increase_size_MB > (LLONG_MAX-meta.virtual_size)/1024/1024)
       {
          fprintf(stderr, "Invalid stored size or requested size increase.\n");
          return 1;
       }
       split_size=meta.split_size;
       if (increase_size_MB > 0) virtual_size = meta.virtual_size + (increase_size_MB * 1024 * 1024);
       if (virtual_size<=meta.virtual_size) virtual_size=meta.virtual_size;
       if (!validate_storage_sizes()) return 1;

       // if virtual size was changed, write it to main file
       if (meta.virtual_size!=virtual_size)
       {
          if (!write_metadata(mainfile))
          {
             printf("cannot update header metadata for new virtual size in file %s\n", storage_file);
             return 1;
          }
       }
    }
    else // file does not exist yet, attempt to create it
    {
       if (errno != ENOENT) { perror(storage_file); return 1; }
       if (!validate_storage_sizes()) return 1;

       mainfile = fopen(storage_file, "w+");
       if (mainfile == NULL)
       {
          printf("cannot open %s for writing\n", storage_file);
          return 1;
       }

       if (fwrite(header,sizeof(header),1,mainfile) != 1 || !write_metadata(mainfile))
       {
          printf("cannot write to %s\n", storage_file);
          return 1;
       }
    }
    if (fsync(fileno(mainfile)) || fclose(mainfile)) { perror(storage_file); return 1; }
    utime(storage_file,NULL);

    char storage_file_path[4096];

    for (int i=0; i<max_files; i++)
    {
       pthread_mutex_init(&storage_locks[i], NULL);
       storage_dirty[i]=1;
       if (snprintf(storage_file_path, sizeof(storage_file_path), "%s.%i", storage_file, i) >= sizeof(storage_file_path))
       {
          fprintf(stderr, "Storage file path is too long.\n");
          return 1;
       }

       // open existing changes file
       files[i] = fopen(storage_file_path, "r+");
       if (files[i] != NULL)
       {
          struct metaStruct meta = {};

          // check version and other parameters
          if (fseeko(files[i], meta_header_offset, SEEK_SET) || fread(&meta,sizeof(meta),1,files[i]) != 1)
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
             if (!write_metadata(files[i]))
             {
                printf("cannot update header metadata for new virtual size in file %s\n", storage_file_path);
                return 1;
             }
          }

          // calculate new last_block_offsets after index of offsets
          if (fseeko(files[i], 0, SEEK_END)) { perror(storage_file_path); return 1; }
          off_t written_data_size = ftello(files[i]) - header_size - offset_block_size;
          if (written_data_size < 0)
          {
             fprintf(stderr, "Truncated index in %s\n", storage_file_path);
             return 1;
          }
          written_data_size += (DATA_BLOCK_SIZE-written_data_size%DATA_BLOCK_SIZE)%DATA_BLOCK_SIZE;
          last_block_offsets[i] = header_size+offset_block_size+written_data_size-DATA_BLOCK_SIZE;
       }
       else // file does not exist yet, attempt to create it
       {
          if (errno != ENOENT) { perror(storage_file_path); return 1; }

          files[i] = fopen(storage_file_path, "w+");
          if (files[i] == NULL)
          {
             printf("cannot open %s for writing\n", storage_file);
             return 1;
          }

          if (fwrite(header,sizeof(header),1,files[i]) != 1 || !write_metadata(files[i]))
          {
             printf("cannot write to %s\n", storage_file_path);
             return 1;
          }

          last_block_offsets[i] = header_size+offset_block_size-DATA_BLOCK_SIZE;
          // Extend the complete index before allowing mmap access.
          if (fseeko(files[i],header_size+offset_block_size-1, SEEK_SET) || fwrite("\0",1,1,files[i]) != 1 || fflush(files[i]))
          {
             perror(storage_file_path);
             return 1;
          }
       }

       indexes[i] = mmap(NULL, header_size + offset_block_size, PROT_READ|PROT_WRITE, MAP_SHARED, fileno(files[i]), 0);
       if (indexes[i] == MAP_FAILED)
       {
          perror("Cannot map storage index");
          return 1;
       }
    }

    // The following line ensures that the process is not killed by systemd
    // on shutdown, it is necessary to keep process running if root filesystem
    // is mounted using dynfilefs. Proper end of the process is umount, not kill.
    argv[0][0] = '@';

    // Allow large transfers without changing storage blocks or writeback semantics.
    char *fuse_argv[]={argv[0], mount_dir, "-o", "max_read=1048576", debug ? "-d" : NULL, NULL};
    return fuse_main(debug ? 5 : 4, fuse_argv, &dynfilefs_oper, NULL);
}
