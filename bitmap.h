#ifndef DYNFILEFS_BITMAP_H
#define DYNFILEFS_BITMAP_H

#include <stddef.h>
#include <stdint.h>

struct block_map
{
    uint64_t *base, *checkpoint, *used, *free_bits;
    size_t words, capacity;
    uint64_t slots;
    size_t *free_next, *free_prev, *dirty_words;
    unsigned char *free_run, *dirty_flags;
    size_t free_heads[64], free_tails[64], free_count, dirty_count;
    uint64_t free_bins;
};

// The caller serializes access and bounds every range by slots.
int map_init(struct block_map *map, const uint64_t *bits, uint64_t slots);
void map_destroy(struct block_map *map);
int map_grow(struct block_map *map, uint64_t slots);
void map_make_available(struct block_map *map, uint64_t first, uint64_t count);
size_t map_take(struct block_map *map, size_t maximum, uint64_t *first);
size_t map_take_at(struct block_map *map, uint64_t first, size_t maximum);
// Reserve only slots not previously exposed by map_make_available().
void map_reserve(struct block_map *map, uint64_t first, uint64_t count);
void map_release(struct block_map *map, uint64_t first, uint64_t count);
int map_protected(const struct block_map *map, uint64_t slot);
void map_checkpoint(struct block_map *map);

// Test a slot within the caller's bitmap bounds.
static inline int map_test(const uint64_t *bits, uint64_t slot)
{
    return !!(bits[slot/64] & (UINT64_C(1)<<(slot%64)));
}

#endif
