#define _GNU_SOURCE
#include "format.h"
#include "bitmap.h"
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// Decode only the released format-500 header; no legacy format is mounted directly.
static int legacy_header(int fd, uint64_t *split, uint64_t *size)
{
    uint64_t header[3];
    char magic[8];
    struct stat st;
    if (fstat(fd, &st)) return -errno;
    if (st.st_size < DYN_BLOCK_SIZE) return -EIO;
    int result=read_at(fd, magic, sizeof(magic), 0);
    if (!result) result=read_at(fd, header, sizeof(header), 2048);
    if (result) return result;
    if (!memcmp(magic, "DYNFS", 5)) return -EPROTONOSUPPORT;
    uint64_t version=le64toh(header[0]);
    if (version!=500)
    {
        fprintf(stderr, "Conversion requires storage format 500 (found %llu).\n", (unsigned long long)version);
        return -EPROTONOSUPPORT;
    }
    *split=le64toh(header[1]);
    *size=le64toh(header[2]);
    return 0;
}

// Own the possible output names durably before creating them, so retries are safe.
static int prepare_conversion(const char *path, const char *header, int first, int count)
{
    char name[PATH_MAX], marker[PATH_MAX], saved[DYN_BLOCK_SIZE];
    int fd=open_suffix(path, ".migrate", O_RDONLY), result=0, retry=fd>=0;
    if (retry)
    {
        struct stat st;
        if (fstat(fd, &st)) result=-errno;
        if (!result && st.st_size!=DYN_BLOCK_SIZE) result=-EIO;
        if (!result) result=read_at(fd, saved, sizeof(saved), 0);
        if (!result && memcmp(saved, header, sizeof(saved))) result=-EIO;
        close(fd);
        if (result) return result;
    }
    else if (fd!=-ENOENT) return fd;
    for (int part=first; part < first+count; part++)
    {
        char suffix[32];
        snprintf(suffix, sizeof(suffix), ".%d", part);
        result=storage_name(name, path, suffix);
        if (result) return result;
        if (retry)
        {
            if (unlink(name) && errno!=ENOENT) return -errno;
        }
        else
        {
            struct stat st;
            if (!lstat(name, &st)) return -EEXIST;
            if (errno!=ENOENT) return -errno;
        }
    }
    if (retry) return sync_directory(path);
    fd=open_suffix(path, ".next", O_RDWR|O_CREAT|O_TRUNC);
    if (fd < 0) return fd;
    result=write_at(fd, header, DYN_BLOCK_SIZE, 0);
    if (!result && fsync(fd)) result=-errno;
    if (close(fd) && !result) result=-errno;
    if (!result) result=storage_name(name, path, ".next");
    if (!result) result=storage_name(marker, path, ".migrate");
    if (!result && rename(name, marker)) result=-errno;
    if (!result) result=sync_directory(path);
    return result;
}

// The caller holds the stable storage lock through conversion and the following mount.
int migration_convert(const char *path)
{
    struct snapshot snapshot={0};
    struct block_map map={0};
    uint64_t split, size, index_size, data_start, part_slots, output_position=0;
    uint64_t *lengths=NULL;
    char header[DYN_BLOCK_SIZE];
    int source=-1, output=-1, result=0, parts=0, created=0, publishing=0;
    const char *companions[]={".work", ".log", ".snapshots"};
    for (size_t i=0; i < sizeof(companions)/sizeof(*companions); i++)
    {
        int fd=open_suffix(path, companions[i], O_RDONLY);
        if (fd >= 0) { close(fd); result=-EEXIST; goto done; }
        if (fd!=-ENOENT) { result=fd; goto done; }
    }
    source=open_suffix(path, "", O_RDONLY);
    if (source < 0) { result=source; goto done; }
    result=legacy_header(source, &split, &size);
    if (!result) result=read_at(source, header, sizeof(header), 0);
    if (result) goto done;
    if (!split || split > INT64_MAX || !size || size > INT64_MAX || size%DYN_BLOCK_SIZE)
    { result=-EINVAL; goto done; }
    if (split%(2*1024*1024))
    {
        fprintf(stderr, "Format-500 conversion requires an even split size in MiB.\n");
        result=-EINVAL; goto done;
    }
    index_size=split/DYN_BLOCK_SIZE*8;
    data_start=DYN_BLOCK_SIZE+index_size;
    if (split > INT64_MAX-data_start || (size-1)/split >= DYN_MAX_PARTS)
    { result=-EOVERFLOW; goto done; }
    snapshot.size=size;
    snapshot.part_size=split+data_start;
    uint64_t roots=(size-1)/(DYN_BLOCK_SIZE*DYN_LEAF_ENTRIES)+1;
    part_slots=snapshot.part_size/DYN_BLOCK_SIZE;
    parts=(int)((size-1)/split+1);
    if (roots > SIZE_MAX/8 || (uint64_t)parts > UINT64_MAX/part_slots)
    { result=-EOVERFLOW; goto done; }
    snapshot.root_count=(size_t)roots;
    snapshot.root=calloc(snapshot.root_count, sizeof(*snapshot.root));
    lengths=malloc((size_t)parts*sizeof(*lengths));
    if (!snapshot.root || !lengths) { result=-ENOMEM; goto done; }
    close(source); source=-1;

    // Validate all part headers and complete indexes before creating new files.
    for (int part=0; part < parts; part++)
    {
        char suffix[32];
        snprintf(suffix, sizeof(suffix), ".%d", part);
        source=open_suffix(path, suffix, O_RDONLY);
        if (source < 0) { result=source; goto done; }
        uint64_t stored_split, stored_size;
        result=legacy_header(source, &stored_split, &stored_size);
        if (result) goto done;
        struct stat st;
        if (fstat(source, &st)) { result=-errno; goto done; }
        if (stored_split!=split || stored_size!=size || st.st_size < 0)
        { result=-EINVAL; goto done; }
        lengths[part]=(uint64_t)st.st_size;
        if (lengths[part] < data_start || lengths[part] > snapshot.part_size)
        { result=-EIO; goto done; }
        close(source); source=-1;
    }
    uint64_t outputs=(roots-1)/part_slots+1;
    if (outputs > (uint64_t)(DYN_MAX_PARTS-parts)) outputs=DYN_MAX_PARTS-parts;
    result=prepare_conversion(path, header, parts, (int)outputs);
    if (result) goto done;
    result=map_init(&map, NULL, (uint64_t)(parts-1)*part_slots+(lengths[parts-1]+4095)/4096);
    if (result) goto done;
    for (int part=0; part < parts; part++)
        map_reserve(&map, (uint64_t)part*part_slots, data_start/DYN_BLOCK_SIZE);

    for (int part=0; part < parts; part++)
    {
        char suffix[32];
        snprintf(suffix, sizeof(suffix), ".%d", part);
        source=open_suffix(path, suffix, O_RDONLY);
        if (source < 0) { result=source; goto done; }
        uint64_t bytes=size-(uint64_t)part*split;
        if (bytes > split) bytes=split;
        for (uint64_t block=0; block < bytes/DYN_BLOCK_SIZE; block+=DYN_LEAF_ENTRIES)
        {
            uint64_t offsets[DYN_LEAF_ENTRIES], leaf[DYN_LEAF_ENTRIES]={0};
            size_t count=(size_t)(bytes/DYN_BLOCK_SIZE-block);
            if (count > DYN_LEAF_ENTRIES) count=DYN_LEAF_ENTRIES;
            result=read_at(source, offsets, count*8, (off_t)(DYN_BLOCK_SIZE+block*8));
            if (result) goto done;
            int present=0;
            for (size_t i=0; i < count; i++)
            {
                uint64_t offset=le64toh(offsets[i]);
                if (!offset) continue;
                if (offset < data_start || offset >= lengths[part] || offset%DYN_BLOCK_SIZE)
                { result=-EIO; goto done; }
                uint64_t slot=(uint64_t)part*part_slots+offset/DYN_BLOCK_SIZE;
                if (map_test(map.used, slot)) { result=-EIO; goto done; }
                map_reserve(&map, slot, 1);
                leaf[i]=htole64(slot+1);
                present=1;
            }
            if (!present) continue;
            if (output < 0 || output_position==snapshot.part_size)
            {
                if (output >= 0)
                {
                    if (fsync(output)) { result=-errno; goto done; }
                    if (close(output)) { output=-1; result=-errno; goto done; }
                    output=-1;
                }
                if (parts+created >= DYN_MAX_PARTS || (uint64_t)(parts+created+1) > UINT64_MAX/part_slots)
                { result=-EOVERFLOW; goto done; }
                snprintf(suffix, sizeof(suffix), ".%d", parts+created);
                output=open_suffix(path, suffix, O_RDWR|O_CREAT|O_EXCL);
                if (output < 0) { result=output; goto done; }
                created++;
                output_position=0;
            }
            uint64_t slot=(uint64_t)(parts+created-1)*part_slots+output_position/DYN_BLOCK_SIZE;
            if (slot==UINT64_MAX) { result=-EOVERFLOW; goto done; }
            result=map_grow(&map, slot+1);
            if (!result) result=write_at(output, leaf, sizeof(leaf), (off_t)output_position);
            if (result) goto done;
            map_reserve(&map, slot, 1);
            snapshot.root[((uint64_t)part*split/DYN_BLOCK_SIZE+block)/DYN_LEAF_ENTRIES]=slot+1;
            output_position+=DYN_BLOCK_SIZE;
        }
        close(source); source=-1;
    }
    if (output >= 0 && fsync(output)) { result=-errno; goto done; }
    result=sync_directory(path);
    if (result) goto done;
    snapshot.slots=map.slots;
    snapshot.bits=map.used;
    // A post-rename error must never delete parts referenced by the published root.
    publishing=1;
    result=snapshot_publish(path, &snapshot);

done:
    if (source >= 0) close(source);
    if (output >= 0) close(output);
    if (result && !publishing)
        for (int part=0; part < created; part++)
        {
            char suffix[32], name[PATH_MAX];
            snprintf(suffix, sizeof(suffix), ".%d", parts+part);
            if (!storage_name(name, path, suffix)) unlink(name);
        }
    snapshot.bits=NULL;
    snapshot_free(&snapshot);
    map_destroy(&map);
    free(lengths);
    return result;
}
