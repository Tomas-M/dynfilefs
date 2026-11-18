#ifndef DYNFILEFS_DURABLE_H
#define DYNFILEFS_DURABLE_H

#include <stddef.h>
#include <stdint.h>
#include <limits.h>

int durable_write(int directory, const char *prefix, const char *name, const void *bytes, size_t length, int padding, int64_t modified, int *published);
int durable_reset(int directory, const char *prefix, const char *name);
int durable_remove(int directory, const char *prefix, const char *name);
int durable_read(int directory, const char *prefix, const char *name, unsigned char **bytes, size_t *length);
int durable_pending(int directory, const char *prefix, char name[PATH_MAX], unsigned char **bytes, size_t *length);
int durable_recover(int directory, const char *prefix);
int durable_unlink(int directory, const char *name);
int durable_rmdir(int directory, const char *name);
int durable_command(int argc, char **argv);

#endif
