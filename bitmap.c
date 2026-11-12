#include "bitmap.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

// Select a nonempty range contained in one bitmap word.
static uint64_t word_mask(unsigned start, unsigned count)
{
    return (UINT64_MAX>>(64-count))<<start;
}

// Allocate all replacement arrays before changing the existing map.
int map_grow(struct block_map *map, uint64_t slots)
{
    if (slots <= map->slots) return 0;
    uint64_t required=(slots-1)/64+1;
    if (required > SIZE_MAX/sizeof(uint64_t) || required > SIZE_MAX/sizeof(size_t)) return -EOVERFLOW;
    size_t words=(size_t)required, capacity=map->capacity ? map->capacity : 1;
    while (capacity < words && capacity <= SIZE_MAX/2) capacity*=2;
    if (capacity < words || capacity > SIZE_MAX/sizeof(uint64_t) || capacity > SIZE_MAX/sizeof(size_t)) capacity=words;
    if (words > map->capacity)
    {
        uint64_t *base=calloc(capacity, sizeof(*base));
        uint64_t *checkpoint=calloc(capacity, sizeof(*checkpoint));
        uint64_t *used=calloc(capacity, sizeof(*used));
        uint64_t *free_bits=calloc(capacity, sizeof(*free_bits));
        size_t *free_words=malloc(capacity*sizeof(*free_words));
        if (!base || !checkpoint || !used || !free_bits || !free_words)
        {
            free(base); free(checkpoint); free(used); free(free_bits); free(free_words);
            return -ENOMEM;
        }
        if (map->words)
        {
            memcpy(base, map->base, map->words*sizeof(*base));
            memcpy(checkpoint, map->checkpoint, map->words*sizeof(*checkpoint));
            memcpy(used, map->used, map->words*sizeof(*used));
            memcpy(free_bits, map->free_bits, map->words*sizeof(*free_bits));
            memcpy(free_words, map->free_words, map->free_count*sizeof(*free_words));
        }
        free(map->base); free(map->checkpoint); free(map->used); free(map->free_bits); free(map->free_words);
        map->base=base; map->checkpoint=checkpoint; map->used=used;
        map->free_bits=free_bits; map->free_words=free_words;
        map->capacity=capacity;
    }
    map->words=words;
    map->slots=slots;
    return 0;
}

// Begin with identical baseline, checkpoint and working occupancy, but no free extents yet.
int map_init(struct block_map *map, const uint64_t *bits, uint64_t slots)
{
    memset(map, 0, sizeof(*map));
    int result=map_grow(map, slots);
    if (result || !bits || !slots) return result;
    memcpy(map->base, bits, map->words*sizeof(*bits));
    if (slots%64) map->base[map->words-1]&=word_mask(0, slots%64);
    memcpy(map->used, map->base, map->words*sizeof(*bits));
    memcpy(map->checkpoint, map->base, map->words*sizeof(*bits));
    return 0;
}

// Release all session-only allocator state.
void map_destroy(struct block_map *map)
{
    free(map->base); free(map->checkpoint); free(map->used); free(map->free_bits); free(map->free_words);
    memset(map, 0, sizeof(*map));
}

// Expose existing physical slots while preserving all three states' allocations.
void map_make_available(struct block_map *map, uint64_t first, uint64_t count)
{
    while (count)
    {
        size_t word=(size_t)(first/64);
        unsigned length=(unsigned)(count < 64-first%64 ? count : 64-first%64);
        uint64_t bits=word_mask(first%64, length) & ~(map->base[word]|map->checkpoint[word]|map->used[word]);
        if (bits && !map->free_bits[word]) map->free_words[map->free_count++]=word;
        map->free_bits[word]|=bits;
        first+=length;
        count-=length;
    }
}

// Take a contiguous run from a known nonempty word, without scanning the map.
size_t map_take(struct block_map *map, size_t maximum, uint64_t *first)
{
    if (!maximum || !map->free_count) return 0;
    size_t word=map->free_words[map->free_count-1];
    unsigned start=(unsigned)__builtin_ctzll(map->free_bits[word]);
    uint64_t bits=map->free_bits[word]>>start;
    unsigned count=bits==UINT64_MAX ? 64 : (unsigned)__builtin_ctzll(~bits);
    if (count > maximum) count=(unsigned)maximum;
    bits=word_mask(start, count);
    map->used[word]|=bits;
    map->free_bits[word]&=~bits;
    if (!map->free_bits[word]) map->free_count--;
    *first=(uint64_t)word*64+start;
    return count;
}

// Reserve newly appended slots which have not been exposed as available.
void map_reserve(struct block_map *map, uint64_t first, uint64_t count)
{
    while (count)
    {
        unsigned length=(unsigned)(count < 64-first%64 ? count : 64-first%64);
        map->used[first/64]|=word_mask(first%64, length);
        first+=length;
        count-=length;
    }
}

// Retire working references without releasing baseline or checkpoint pins.
void map_release(struct block_map *map, uint64_t first, uint64_t count)
{
    while (count)
    {
        size_t word=(size_t)(first/64);
        unsigned length=(unsigned)(count < 64-first%64 ? count : 64-first%64);
        uint64_t mask=word_mask(first%64, length), bits=mask & ~(map->base[word]|map->checkpoint[word]);
        map->used[word]&=~mask;
        if (bits && !map->free_bits[word]) map->free_words[map->free_count++]=word;
        map->free_bits[word]|=bits;
        first+=length;
        count-=length;
    }
}

// A block referenced by either recovery state requires copy-on-write.
int map_protected(const struct block_map *map, uint64_t slot)
{
    return map_test(map->base, slot) || map_test(map->checkpoint, slot);
}

// Pin the published working state; the caller then re-exposes real physical extents.
void map_checkpoint(struct block_map *map)
{
    if (map->words)
    {
        memcpy(map->checkpoint, map->used, map->words*sizeof(*map->used));
        memset(map->free_bits, 0, map->words*sizeof(*map->free_bits));
    }
    map->free_count=0;
}
