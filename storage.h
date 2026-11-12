#ifndef DYNFILEFS_STORAGE_H
#define DYNFILEFS_STORAGE_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define DYN_BLOCK_SIZE 4096
#define DYN_LEAF_ENTRIES 512
#define DYN_FORMAT 600
#define DYN_MAX_PARTS 9999

struct storage;

int storage_open(struct storage **result, const char *path, uint64_t size, uint64_t increase, uint64_t split, int norollback);
void storage_close(struct storage *storage);
uint64_t storage_size(struct storage *storage);
int storage_read(struct storage *storage, void *buffer, size_t size, off_t offset);
int storage_write(struct storage *storage, const void *buffer, size_t size, off_t offset);
int storage_read_extent(struct storage *storage, off_t offset, size_t size, int *fd, off_t *position);
int storage_sync(struct storage *storage);
// Finalize a stopped mount; afterward only retry finish or close the instance.
int storage_finish(struct storage *storage);
int storage_set_rollback(struct storage *storage, int enabled);
int storage_get_rollback(struct storage *storage);

#endif
