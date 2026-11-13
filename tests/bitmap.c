#include "../bitmap.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#ifdef TEST_ALLOC_FAILURE
static unsigned failure_after;
void *__real_malloc(size_t size);
void *__real_calloc(size_t count, size_t size);
void *__wrap_malloc(size_t size);
void *__wrap_calloc(size_t count, size_t size);

// Fail one chosen allocation while keeping libc behavior for all others.
void *__wrap_malloc(size_t size)
{
    if (failure_after && !--failure_after) return NULL;
    return __real_malloc(size);
}

// Exercise the same failure points for zero-initialized bitmap arrays.
void *__wrap_calloc(size_t count, size_t size)
{
    if (failure_after && !--failure_after) return NULL;
    return __real_calloc(count, size);
}

// Failed growth must leave all three states and the available-slot index intact.
static void check_failures(void)
{
    struct block_map map;
    uint64_t original=1, first;
    assert(!map_init(&map, &original, 64));
    map_pin_read(&map, 0, 1);
    map_make_available(&map, 0, 64);
    assert(map_take_at(&map, 1, 1)==1);
    map_release(&map, 1, 1);
    struct block_map before=map;
    for (unsigned allocation=1; allocation <= 10; allocation++)
    {
        failure_after=allocation;
        assert(map_grow(&map, 1024)==-ENOMEM);
        assert(!memcmp(&map, &before, sizeof(map)));
        assert(map.read_pins[0]==1 && map.checkpoint[0]==1 && map.used[0]==1 && map.free_bits[0]==~UINT64_C(1));
        assert(map.dirty_count==1 && map.dirty_flags[0] && map.dirty_words[0]==0);
    }
    assert(map_take(&map, 64, &first)==63 && first==1);
    assert(!map_grow(&map, 1024));
    map_destroy(&map);
}
#endif

// COW must preserve the descriptor pins even after its working references are retired.
static void check_snapshots(void)
{
    uint64_t bits[]={UINT64_C(1)|(UINT64_C(1)<<63), UINT64_C(2)};
    struct block_map map, next;
    assert(!map_init(&map, bits, 100));
    map_pin_read(&map, 0, 1); map_pin_read(&map, 63, 1); map_pin_read(&map, 65, 1);
    map_make_available(&map, 0, 100);
    map_release(&map, 0, 1);
    unsigned char allocated[100]={0};
    uint64_t first;
    size_t count, total=0;
    while ((count=map_take(&map, 19, &first)))
    {
        assert(first+count <= 100 && count <= 19);
        for (size_t i=0; i < count; i++)
        {
            assert(!map_protected(&map, first+i) && !allocated[first+i]);
            assert(map_test(map.used, first+i));
            allocated[first+i]=1;
        }
        total+=count;
    }
    assert(total==97 && !map_test(map.used, 0) && map_protected(&map, 0));
    assert(!map_init(&next, map.used, map.slots));
    map_make_available(&next, 0, 100);
    assert(map_take(&next, 64, &first)==1 && first==0);
    assert(!map_take(&next, 1, &first));
    map_destroy(&next);
    map_destroy(&map);
}

// Replaced checkpoints release old blocks while descriptor references remain pinned.
static void check_checkpoints(void)
{
    struct block_map map;
    uint64_t original=1, first;
    assert(!map_init(&map, &original, 5));
    map_pin_read(&map, 0, 1);
    map_make_available(&map, 0, 5);
    assert(map_take(&map, 1, &first)==1 && first==1);
    map_release(&map, 0, 1);
    map_checkpoint(&map);
    assert(map.free_count==1 && map_protected(&map, 0) && map_protected(&map, 1));
    map_release(&map, 1, 1);
    assert(map_take(&map, 1, &first)==1 && first==2);
    assert(!map_grow(&map, 130));
    assert(map_protected(&map, 0) && map_protected(&map, 1));
    map_reserve(&map, 128, 1);
    map_checkpoint(&map);
    assert(map_protected(&map, 0) && !map_protected(&map, 1));
    assert(map_protected(&map, 2) && map_protected(&map, 128));
    assert(map_take(&map, 1, &first)==1 && first==1);
    map_release(&map, 2, 1);
    map_release(&map, 128, 1);
    map_checkpoint(&map);
    assert(map_protected(&map, 0) && map_protected(&map, 1));
    assert(map_take(&map, 64, &first)==3 && first==2);
    assert(map_take(&map, 64, &first)==1 && first==128);
    assert(!map_take(&map, 64, &first));
    assert(map.read_pins[0]==1);
    map_destroy(&map);
}

// Growth alone must not expose gaps or slots beyond the end of a physical file.
static void check_growth(void)
{
    struct block_map map;
    uint64_t first;
    assert(!map_init(&map, NULL, 0));
    assert(!map_take(&map, 64, &first));
    assert(!map_grow(&map, 60));
    map_reserve(&map, 0, 60);
    assert(!map_grow(&map, 130));
    assert(!map_take(&map, 64, &first));
    map_reserve(&map, 60, 70);
    map_release(&map, 60, 70);
    size_t count, total=0;
    unsigned char seen[130]={0};
    while ((count=map_take(&map, 100, &first)))
        for (size_t i=0; i < count; i++)
        {
            assert(first+i >= 60 && first+i < 130 && !seen[first+i]);
            seen[first+i]=1;
            total++;
        }
    assert(total==70);
    for (uint64_t i=0; i < 130; i++) assert(map_test(map.used, i));
    map_destroy(&map);
}

// Existing images need no spare capacity; large growing maps reserve at most one eighth.
static void check_capacity(void)
{
    struct block_map map;
    assert(!map_init(&map, NULL, 3*64-1));
    assert(map.words==3 && map.capacity==3);
    assert(!map_grow(&map, 4*64) && map.capacity==6);
    map_destroy(&map);
    assert(!map_init(&map, NULL, 4095*64));
    assert(map.capacity==4095);
    assert(!map_grow(&map, 4096*64) && map.capacity==4096);
    map_destroy(&map);
    assert(!map_init(&map, NULL, 4097*64-7));
    assert(map.words==4097 && map.capacity==4097);
    map_reserve(&map, 4096*64, 1);
    for (unsigned i=0; i < 12; i++)
    {
        size_t words=map.capacity+1;
        assert(!map_grow(&map, (uint64_t)words*64));
        assert(map.capacity >= words && map.capacity <= words+words/8);
        assert(map.used[4096]==1 && map.dirty_count==1 && map.dirty_flags[4096]);
    }
    if (SIZE_MAX<=UINT32_MAX)
    {
        struct block_map before=map;
        assert(map_grow(&map, UINT64_MAX)==-EOVERFLOW);
        assert(!memcmp(&map, &before, sizeof(map)));
    }
    map_destroy(&map);
}

// Overlapping availability and repeated release must not enqueue a word twice.
static void check_reuse(void)
{
    struct block_map map;
    uint64_t first;
    assert(!map_init(&map, NULL, 257));
    map_make_available(&map, 0, 64);
    map_make_available(&map, 0, 32);
    assert(map.free_count==1 && map_take(&map, 128, &first)==64 && first==0);
    for (unsigned i=0; i < 10000; i++)
    {
        unsigned slot=(i*73)%64;
        map_release(&map, slot, 1);
        map_release(&map, slot, 1);
        map_make_available(&map, slot, 1);
        assert(map.free_count==1 && map_take(&map, 64, &first)==1 && first==slot);
        assert(!map_take(&map, 64, &first));
    }
    map_make_available(&map, 256, 1);
    assert(map_take(&map, 64, &first)==1 && first==256);
    assert(!map_take(&map, 1, &first));
    map_destroy(&map);
}

// Long requests must find large runs even behind thousands of isolated free slots.
static void check_fragmentation(void)
{
    struct block_map map;
    uint64_t first;
    assert(!map_init(&map, NULL, 1024*64+512));
    for (size_t word=0; word < 1024; word++) map_make_available(&map, word*64+1, 1);
    map_make_available(&map, 1024*64, 512);
    assert(map_take(&map, 1, &first)==1 && first==1);
    assert(map_take(&map, 256, &first)==256 && first==1024*64);
    assert(map_take(&map, 256, &first)==256 && first==1024*64+256);
    assert(map_take(&map, 256, &first)==1 && first==65);
    map_destroy(&map);

    assert(!map_init(&map, NULL, 256));
    map_make_available(&map, 2, 1);
    map_make_available(&map, 10, 20);
    map_make_available(&map, 80, 40);
    assert(map_take(&map, 16, &first)==16 && first==10);
    assert(map_take(&map, 32, &first)==32 && first==80);
    assert(!map_take_at(&map, 200, 56));
    map_destroy(&map);

    assert(!map_init(&map, NULL, 256));
    map_make_available(&map, 60, 90);
    assert(map_take(&map, 90, &first)==90 && first==60);
    assert(!map_take(&map, 1, &first));
    map_destroy(&map);
}

// Net-zero changes remain queued once; checkpoints must not expose unallocated gaps.
static void check_dirty_words(void)
{
    struct block_map map;
    uint64_t first;
    assert(!map_init(&map, NULL, 128*1024+8));
    map_make_available(&map, 1, 7);
    assert(map_take_at(&map, 1, 1)==1);
    map_release(&map, 1, 1);
    assert(map_take_at(&map, 2, 1)==1);
    map_reserve(&map, 128*1024, 1);
    assert(map.dirty_count==2 && map.dirty_words[0]==0 && map.dirty_words[1]==2048);
    assert(!map_grow(&map, 256*1024));
    assert(map.dirty_count==2 && map.dirty_flags[0] && map.dirty_flags[2048]);
    map_checkpoint(&map);
    assert(!map.dirty_count && !map.dirty_flags[0] && !map.dirty_flags[2048]);
    assert(map.checkpoint[0]==4 && map.checkpoint[2048]==1 && !map.checkpoint[2049]);
    map_release(&map, 2, 1);
    map_release(&map, 128*1024, 1);
    map_checkpoint(&map);
    assert(map_take(&map, 256*1024, &first)==7 && first==1);
    assert(map_take(&map, 256*1024, &first)==1 && first==128*1024);
    assert(!map_take(&map, 1, &first));
    map_checkpoint(&map);
    assert(!map.dirty_count && !map.free_count);
    map_checkpoint(&map);
    assert(!map.dirty_count && !map.free_count);
    map_destroy(&map);
}

// Check the run-bin links independently of the order chosen by the allocator.
static void check_bins(const struct block_map *map)
{
    unsigned char seen[8]={0};
    size_t total=0;
    assert(map->words==8);
    for (size_t bin=0; bin < 64; bin++)
    {
        size_t previous=0;
        assert(!!map->free_heads[bin]==!!(map->free_bins & (UINT64_C(1)<<bin)));
        for (size_t link=map->free_heads[bin]; link; link=map->free_next[link-1])
        {
            size_t word=link-1;
            assert(word < map->words && !seen[word]++);
            assert(map->free_prev[word]==previous && map->free_run[word]==bin+1);
            unsigned longest=0, run=0;
            for (unsigned bit=0; bit < 64; bit++)
            {
                if (map->free_bits[word] & (UINT64_C(1)<<bit)) run++;
                else run=0;
                if (run > longest) longest=run;
            }
            assert(longest==bin+1);
            previous=link;
            total++;
        }
        assert(previous==map->free_tails[bin]);
    }
    assert(total==map->free_count);
    for (size_t word=0; word < map->words; word++) assert(!!seen[word]==!!map->free_bits[word]);
}

// Compare random mutations with a per-slot model, including physical gaps and all pins.
static void check_randomized(void)
{
    enum { SLOTS=512 };
    struct block_map map;
    uint64_t bits[8]={0};
    unsigned char physical[SLOTS], pins[SLOTS], checkpoint[SLOTS], used[SLOTS], dirty[8]={0};
    for (size_t i=0; i < SLOTS; i++)
    {
        physical[i]=i%128 < 96;
        pins[i]=checkpoint[i]=used[i]=physical[i] && i%23==0;
        if (pins[i]) bits[i/64]|=UINT64_C(1)<<(i%64);
    }
    assert(!map_init(&map, bits, SLOTS));
    for (size_t i=0; i < SLOTS; i++)
        if (pins[i]) map_pin_read(&map, i, 1);
    for (size_t i=0; i < SLOTS; i+=128) map_make_available(&map, i, 96);
    uint32_t seed=7;
    for (unsigned step=0; step < 20000; step++)
    {
        seed=seed*1664525+1013904223;
        unsigned operation=(seed>>16)%6, slot=seed%SLOTS, maximum=1+(seed>>8)%130;
        unsigned char before[SLOTS];
        memcpy(before, used, sizeof(before));
        if (operation==0 || operation==1)
        {
            uint64_t first=slot;
            size_t count=operation ? map_take_at(&map, first, maximum) : map_take(&map, maximum, &first);
            assert(count <= maximum && first+count <= SLOTS);
            if (operation)
            {
                size_t expected=0;
                while (expected < maximum && slot+expected < SLOTS)
                {
                    size_t i=slot+expected;
                    if (!physical[i] || pins[i] || checkpoint[i] || used[i]) break;
                    expected++;
                }
                assert(count==expected);
            }
            else if (!count)
                for (size_t i=0; i < SLOTS; i++) assert(!physical[i] || pins[i] || checkpoint[i] || used[i]);
            for (size_t i=first; i < first+count; i++)
            {
                assert(physical[i] && !pins[i] && !checkpoint[i] && !used[i]);
                used[i]=1;
            }
        }
        else if (operation==2)
        {
            if (maximum > SLOTS-slot) maximum=SLOTS-slot;
            map_release(&map, slot, maximum);
            memset(used+slot, 0, maximum);
        }
        else if (operation==3)
        {
            map_checkpoint(&map);
            memcpy(checkpoint, used, sizeof(checkpoint));
            memset(dirty, 0, sizeof(dirty));
        }
        else if (operation==4) map_make_available(&map, slot/128*128, 96);
        else if (!physical[slot])
        {
            physical[slot]=used[slot]=1;
            map_reserve(&map, slot, 1);
        }
        for (size_t i=0; i < SLOTS; i++)
        {
            if (operation!=3 && before[i]!=used[i]) dirty[i/64]=1;
            assert(map_test(map.read_pins, i)==pins[i] && map_test(map.checkpoint, i)==checkpoint[i]);
            assert(map_test(map.used, i)==used[i]);
            assert(map_test(map.free_bits, i)==(physical[i] && !pins[i] && !checkpoint[i] && !used[i]));
        }
        size_t count=0;
        unsigned char listed[8]={0};
        for (size_t word=0; word < 8; word++)
        {
            assert(map.dirty_flags[word]==dirty[word]);
            count+=dirty[word];
        }
        assert(count==map.dirty_count);
        for (size_t i=0; i < map.dirty_count; i++)
        {
            size_t word=map.dirty_words[i];
            assert(word < 8 && dirty[word] && !listed[word]++);
        }
        check_bins(&map);
    }
    map_destroy(&map);
}

// Reusable capacity counts physical free slots and excludes every kind of pin.
static void check_reusable(void)
{
    struct block_map map;
    uint64_t first, snapshots[4]={2};
    assert(!map_init(&map, NULL, 128));
    assert(!map.read_pins[0] && !map_reusable_bytes(&map));
    map_make_available(&map, 0, 128);
    assert(map_reusable_bytes(&map)==128*4096);
    assert(map_take(&map, 1, &first)==1 && first==0);
    map_pin_read(&map, first, 1);
    map_release(&map, first, 1);
    map_checkpoint(&map);
    assert(map_reusable_bytes(&map)==127*4096 && map_protected(&map, first));
    map_set_snapshots(&map, snapshots);
    assert(map_reusable_bytes(&map)==126*4096);
    snapshots[0]=0;
    map_set_snapshots(&map, snapshots);
    assert(map_reusable_bytes(&map)==127*4096);
    assert(!map_grow(&map, 256) && map_reusable_bytes(&map)==127*4096);
    map_destroy(&map);
}

// Exercise allocator invariants without FUSE or backing storage.
int main(void)
{
    check_snapshots();
    check_checkpoints();
    check_growth();
    check_capacity();
    check_reuse();
    check_fragmentation();
    check_dirty_words();
    check_randomized();
    check_reusable();
#ifdef TEST_ALLOC_FAILURE
    check_failures();
#endif
    puts("PASS: bitmap pins, incremental checkpoints, contiguous runs, gaps and randomized recycling");
    return 0;
}
