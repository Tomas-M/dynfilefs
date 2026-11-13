#ifndef DYNFILEFS_STORAGE_H
#define DYNFILEFS_STORAGE_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define DYN_BLOCK_SIZE 4096
#define DYN_LEAF_ENTRIES 512
#define DYN_FORMAT 600
#define DYN_MAGIC "DYNFS600"
#define DYN_NAME_MAX 250
#define DYN_MAX_PARTS 9999
// Bound resident 4 KiB index pages; storage.c partitions them across its I/O locks.
#ifndef DYN_CACHE_PAGES
#define DYN_CACHE_PAGES 8192
#endif

struct storage;
struct storage_snapshot;

struct storage_info
{
    uint64_t version, size, split, reusable, parent;
};

int storage_inspect(const char *path, struct storage_info *info);
int storage_snapshot_inspect(const char *path, int json, char **result);
int storage_open(struct storage **result, const char *path, uint64_t size, uint64_t increase, uint64_t split, const char *restore);
void storage_close(struct storage *storage);
uint64_t storage_size(struct storage *storage);
int storage_read(struct storage *storage, void *buffer, size_t size, off_t offset);
int storage_write(struct storage *storage, const void *buffer, size_t size, off_t offset);
int storage_read_extent(struct storage *storage, off_t offset, size_t size, int *fd, off_t *position);
int storage_sync(struct storage *storage);
// Finalize a stopped mount; afterward only retry finish or close the instance.
int storage_finish(struct storage *storage);
int storage_get_info(struct storage *storage, struct storage_info *info);
int storage_snapshot_create(struct storage *storage, const char *name);
int storage_snapshot_delete(struct storage *storage, const char *name);
int storage_snapshot_list(struct storage *storage, char **result);
int storage_snapshot_report(struct storage *storage, int json, char **result);
int storage_snapshot_stat(struct storage *storage, const char *name, uint64_t *size);
int storage_snapshot_open(struct storage *storage, const char *name, struct storage_snapshot **view);
void storage_snapshot_close(struct storage *storage, struct storage_snapshot *view);
uint64_t storage_snapshot_size(struct storage_snapshot *view);
int storage_snapshot_read(struct storage *storage, struct storage_snapshot *view, void *buffer, size_t size, off_t offset);

#endif
