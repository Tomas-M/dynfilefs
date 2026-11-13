#include "bitmap.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

// Select a nonempty range contained in one bitmap word.
static uint64_t word_mask(unsigned start, unsigned count)
{
    return (UINT64_MAX>>(64-count))<<start;
}

// Queue each changed word once, including changes later undone before a checkpoint.
static void set_used(struct block_map *map, size_t word, uint64_t bits)
{
    if (map->used[word]==bits) return;
    if (!map->dirty_flags[word])
    {
        map->dirty_words[map->dirty_count++]=word;
        map->dirty_flags[word]=1;
    }
    map->used[word]=bits;
}

// Index words by their longest free run; links store word+1 so zero means absent.
static void set_available(struct block_map *map, size_t word, uint64_t bits)
{
    unsigned length=0, previous=map->free_run[word];
    for (uint64_t run=bits; run; run&=run<<1) length++;
    map->free_bits[word]=bits;
    if (length==previous) return;
    if (previous)
    {
        size_t next=map->free_next[word], prev=map->free_prev[word];
        if (prev) map->free_next[prev-1]=next;
        else map->free_heads[previous-1]=next;
        if (next) map->free_prev[next-1]=prev;
        else map->free_tails[previous-1]=prev;
        if (!map->free_heads[previous-1]) map->free_bins&=~(UINT64_C(1)<<(previous-1));
        map->free_count--;
    }
    if (length)
    {
        size_t head=map->free_heads[length-1], tail=map->free_tails[length-1];
        if (!head || word+1 < head)
        {
            map->free_prev[word]=0; map->free_next[word]=head;
            if (head) map->free_prev[head-1]=word+1;
            else map->free_tails[length-1]=word+1;
            map->free_heads[length-1]=word+1;
        }
        else
        {
            map->free_prev[word]=tail; map->free_next[word]=0;
            map->free_next[tail-1]=word+1;
            map->free_tails[length-1]=word+1;
        }
        map->free_bins|=UINT64_C(1)<<(length-1);
        map->free_count++;
    }
    map->free_run[word]=(unsigned char)length;
}

// Allocate all replacement arrays before changing the existing map.
int map_grow(struct block_map *map, uint64_t slots)
{
    if (slots <= map->slots) return 0;
    uint64_t required=(slots-1)/64+1;
    if (required > SIZE_MAX/sizeof(uint64_t) || required > SIZE_MAX/sizeof(size_t)) return -EOVERFLOW;
    size_t words=(size_t)required, capacity=map->capacity ? map->capacity : words;
    // Load existing maps exactly; limit growth reserve once the physical span reaches 1 GiB.
    while (capacity < words)
    {
        size_t growth=capacity < 4096 ? capacity : capacity/8;
        if (capacity < 4096 && growth > 4096-capacity) growth=4096-capacity;
        if (growth > SIZE_MAX-capacity) { capacity=words; break; }
        capacity+=growth;
    }
    if (capacity > SIZE_MAX/sizeof(uint64_t) || capacity > SIZE_MAX/sizeof(size_t)) capacity=words;
    if (words > map->capacity)
    {
        struct block_map grown=*map;
        grown.read_pins=calloc(capacity, sizeof(*grown.read_pins));
        grown.checkpoint=calloc(capacity, sizeof(*grown.checkpoint));
        grown.used=calloc(capacity, sizeof(*grown.used));
        grown.snapshots=calloc(capacity, sizeof(*grown.snapshots));
        grown.free_bits=calloc(capacity, sizeof(*grown.free_bits));
        grown.free_next=calloc(capacity, sizeof(*grown.free_next));
        grown.free_prev=calloc(capacity, sizeof(*grown.free_prev));
        grown.free_run=calloc(capacity, sizeof(*grown.free_run));
        grown.dirty_words=malloc(capacity*sizeof(*grown.dirty_words));
        grown.dirty_flags=calloc(capacity, sizeof(*grown.dirty_flags));
        int missing_maps=!grown.read_pins || !grown.checkpoint || !grown.used || !grown.free_bits;
        int missing_index=!grown.free_next || !grown.free_prev || !grown.free_run;
        if (missing_maps || missing_index || !grown.snapshots || !grown.dirty_words || !grown.dirty_flags)
        {
            map_destroy(&grown);
            return -ENOMEM;
        }
        if (map->words)
        {
            memcpy(grown.read_pins, map->read_pins, map->words*sizeof(*grown.read_pins));
            memcpy(grown.checkpoint, map->checkpoint, map->words*sizeof(*grown.checkpoint));
            memcpy(grown.used, map->used, map->words*sizeof(*grown.used));
            memcpy(grown.snapshots, map->snapshots, map->words*sizeof(*grown.snapshots));
            memcpy(grown.free_bits, map->free_bits, map->words*sizeof(*grown.free_bits));
            memcpy(grown.free_next, map->free_next, map->words*sizeof(*grown.free_next));
            memcpy(grown.free_prev, map->free_prev, map->words*sizeof(*grown.free_prev));
            memcpy(grown.free_run, map->free_run, map->words*sizeof(*grown.free_run));
            memcpy(grown.dirty_words, map->dirty_words, map->dirty_count*sizeof(*grown.dirty_words));
            memcpy(grown.dirty_flags, map->dirty_flags, map->words*sizeof(*grown.dirty_flags));
        }
        grown.capacity=capacity;
        map_destroy(map);
        *map=grown;
    }
    map->words=words;
    map->slots=slots;
    return 0;
}

// Begin with identical checkpoint and working occupancy; descriptor pins start empty.
int map_init(struct block_map *map, const uint64_t *bits, uint64_t slots)
{
    memset(map, 0, sizeof(*map));
    int result=map_grow(map, slots);
    if (result || !bits || !slots) return result;
    memcpy(map->used, bits, map->words*sizeof(*bits));
    if (slots%64) map->used[map->words-1]&=word_mask(0, slots%64);
    memcpy(map->checkpoint, map->used, map->words*sizeof(*bits));
    return 0;
}

// Release all session-only allocator state.
void map_destroy(struct block_map *map)
{
    free(map->read_pins); free(map->checkpoint); free(map->used); free(map->snapshots); free(map->free_bits);
    free(map->free_next); free(map->free_prev); free(map->free_run);
    free(map->dirty_words); free(map->dirty_flags);
    memset(map, 0, sizeof(*map));
}

// Expose only physical slots unreferenced by working, recovery, saved or deferred-read states.
void map_make_available(struct block_map *map, uint64_t first, uint64_t count)
{
    while (count)
    {
        size_t word=(size_t)(first/64);
        unsigned length=(unsigned)(count < 64-first%64 ? count : 64-first%64);
        uint64_t bits=word_mask(first%64, length) & ~(map->read_pins[word]|map->checkpoint[word]|map->used[word]|map->snapshots[word]);
        set_available(map, word, map->free_bits[word]|bits);
        first+=length;
        count-=length;
    }
}

// Measure one physical free run, touching at most the requested number of blocks.
static size_t available_run(const struct block_map *map, uint64_t first, size_t maximum)
{
    if (first >= map->slots) return 0;
    if (maximum > map->slots-first) maximum=(size_t)(map->slots-first);
    size_t length=0;
    while (length < maximum)
    {
        unsigned start=(unsigned)(first%64);
        uint64_t bits=map->free_bits[first/64]>>start;
        unsigned count=bits==UINT64_MAX ? 64 : (unsigned)__builtin_ctzll(~bits);
        if (count > maximum-length) count=(unsigned)(maximum-length);
        length+=count; first+=count;
        if (count < 64-start) break;
    }
    return length;
}

// Reserve an exact adjacent run without disturbing unrelated free ranges.
size_t map_take_at(struct block_map *map, uint64_t first, size_t maximum)
{
    size_t length=available_run(map, first, maximum), remaining=length;
    while (remaining)
    {
        size_t word=(size_t)(first/64);
        unsigned count=(unsigned)(remaining < 64-first%64 ? remaining : 64-first%64);
        uint64_t bits=word_mask(first%64, count);
        set_used(map, word, map->used[word]|bits);
        set_available(map, word, map->free_bits[word]&~bits);
        first+=count; remaining-=count;
    }
    return length;
}

// Prefer adequate runs globally, then extend across adjacent words without scanning holes.
size_t map_take(struct block_map *map, size_t maximum, uint64_t *first)
{
    if (!maximum || !map->free_bins) return 0;
    unsigned wanted=(unsigned)(maximum < 64 ? maximum : 64);
    uint64_t bins=map->free_bins & (UINT64_MAX<<(wanted-1));
    unsigned length=bins ? (unsigned)__builtin_ctzll(bins)+1 : 64-(unsigned)__builtin_clzll(map->free_bins);
    size_t word=map->free_heads[length-1]-1;
    uint64_t bits=map->free_bits[word];
    if (wanted > length) wanted=length;
    unsigned start;
    while (1)
    {
        start=(unsigned)__builtin_ctzll(bits);
        uint64_t run=bits>>start;
        unsigned count=run==UINT64_MAX ? 64 : (unsigned)__builtin_ctzll(~run);
        if (count >= wanted) break;
        bits&=~word_mask(start, count);
    }
    uint64_t position=(uint64_t)word*64+start;
    size_t count=available_run(map, position, maximum);
    // Include a preceding partial word when the selected run cannot fill the request.
    while (count < maximum && position)
    {
        unsigned end=(unsigned)((position-1)%64)+1;
        uint64_t run=map->free_bits[(position-1)/64]<<(64-end);
        unsigned preceding=run==UINT64_MAX ? 64 : (unsigned)__builtin_clzll(~run);
        if (preceding > maximum-count) preceding=(unsigned)(maximum-count);
        position-=preceding; count+=preceding;
        if (preceding < end) break;
    }
    *first=position;
    return map_take_at(map, position, count);
}

// Reserve newly appended slots which have not been exposed as available.
void map_reserve(struct block_map *map, uint64_t first, uint64_t count)
{
    while (count)
    {
        unsigned length=(unsigned)(count < 64-first%64 ? count : 64-first%64);
        set_used(map, (size_t)(first/64), map->used[first/64]|word_mask(first%64, length));
        first+=length;
        count-=length;
    }
}

// Retire working references without releasing recovery, snapshot or deferred-read pins.
void map_release(struct block_map *map, uint64_t first, uint64_t count)
{
    while (count)
    {
        size_t word=(size_t)(first/64);
        unsigned length=(unsigned)(count < 64-first%64 ? count : 64-first%64);
        uint64_t mask=word_mask(first%64, length);
        uint64_t bits=map->used[word] & mask & ~(map->read_pins[word]|map->checkpoint[word]|map->snapshots[word]);
        set_used(map, word, map->used[word]&~mask);
        set_available(map, word, map->free_bits[word]|bits);
        first+=length;
        count-=length;
    }
}

// Every saved, checkpointed or deferred-read reference requires copy-on-write.
int map_protected(const struct block_map *map, uint64_t slot)
{
    return map_test(map->read_pins, slot) || map_test(map->checkpoint, slot) || map_test(map->snapshots, slot);
}

// Update changed checkpoint words and expose only blocks released by the old checkpoint.
void map_checkpoint(struct block_map *map)
{
    for (size_t i=0; i < map->dirty_count; i++)
    {
        size_t word=map->dirty_words[i];
        uint64_t released=map->checkpoint[word] & ~(map->used[word]|map->read_pins[word]|map->snapshots[word]);
        map->checkpoint[word]=map->used[word];
        set_available(map, word, map->free_bits[word]|released);
        map->dirty_flags[word]=0;
    }
    map->dirty_count=0;
}

// Replace snapshot pins after durable deletion, exposing only newly unreferenced slots.
void map_set_snapshots(struct block_map *map, const uint64_t *bits)
{
    for (size_t i=0; i < map->words; i++)
    {
        uint64_t released=map->snapshots[i] & ~(bits[i]|map->used[i]|map->read_pins[i]|map->checkpoint[i]);
        map->snapshots[i]=bits[i];
        set_available(map, i, (map->free_bits[i]|released)&~bits[i]);
    }
}

// Deferred FUSE descriptor replies keep their source bytes immutable until the mount stops.
void map_pin_read(struct block_map *map, uint64_t first, uint64_t count)
{
    while (count)
    {
        unsigned length=(unsigned)(count < 64-first%64 ? count : 64-first%64);
        map->read_pins[first/64]|=word_mask(first%64, length);
        first+=length;
        count-=length;
    }
}

// Count allocator-ready slots, excluding shared and protected blocks and physical-file gaps.
uint64_t map_reusable_bytes(const struct block_map *map)
{
    uint64_t blocks=0;
    for (size_t i=0; i < map->words; i++) blocks+=(unsigned)__builtin_popcountll(map->free_bits[i]);
    return blocks*4096;
}
