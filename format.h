#ifndef DYNFILEFS_FORMAT_H
#define DYNFILEFS_FORMAT_H

#include "storage.h"
#include <limits.h>

struct snapshot
{
    uint64_t size, part_size, slots;
    size_t root_count;
    uint64_t *root, *bits;
    uint64_t sequence;
};

int read_at(int fd, void *buffer, size_t size, off_t offset);
int write_at(int fd, const void *buffer, size_t size, off_t offset);
int storage_name(char *name, const char *path, const char *suffix);
int open_suffix(const char *path, const char *suffix, int flags);
int lock_storage(const char *path);
int unlock_storage(const char *path, int fd);
int sync_directory(const char *path);
uint64_t metadata_checksum(const unsigned char *bytes, size_t size);
int snapshot_length(const struct snapshot *snapshot, size_t *length);
int snapshot_validate(const struct snapshot *snapshot);
int snapshot_load(int fd, struct snapshot *snapshot);
int snapshot_write(int fd, const struct snapshot *snapshot);
int snapshot_replace(const char *path, const char *suffix, const struct snapshot *snapshot, int *published);
int snapshot_publish(const char *path, const struct snapshot *snapshot);
void snapshot_free(struct snapshot *snapshot);
int migration_convert(const char *path);

#endif
