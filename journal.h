#ifndef DYNFILEFS_JOURNAL_H
#define DYNFILEFS_JOURNAL_H

#include "format.h"

#define JOURNAL_LIMIT (UINT64_C(32)*1024*1024)

int journal_append(const char *path, const struct snapshot *snapshot,
                   const uint64_t *previous_bits, size_t previous_words, size_t previous_root_count,
                   const size_t *dirty_roots, size_t dirty_root_count,
                   const size_t *dirty_words, size_t dirty_word_count, int *fd, uint64_t *length);
int journal_replay(const char *path, struct snapshot *snapshot, int *replayed);
int journal_reset(const char *path, int *fd);

#endif
