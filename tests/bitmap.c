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
    map_make_available(&map, 0, 64);
    struct block_map before=map;
    for (unsigned allocation=1; allocation <= 5; allocation++)
    {
        failure_after=allocation;
        assert(map_grow(&map, 1024)==-ENOMEM);
        assert(!memcmp(&map, &before, sizeof(map)));
        assert(map.base[0]==1 && map.checkpoint[0]==1 && map.used[0]==1 && map.free_bits[0]==~UINT64_C(1));
    }
    assert(map_take(&map, 64, &first)==63 && first==1);
    assert(!map_grow(&map, 1024));
    map_destroy(&map);
}
#endif

// COW must preserve the baseline even after its working references are retired.
static void check_snapshots(void)
{
    uint64_t bits[]={UINT64_C(1)|(UINT64_C(1)<<63), UINT64_C(2)};
    struct block_map map, next;
    assert(!map_init(&map, bits, 100));
    map_make_available(&map, 0, 100);
    map_release(&map, 0, 1);
    unsigned char allocated[100]={0};
    uint64_t first;
    size_t count, total=0;
    while ((count=map_take(&map, 19, &first)))
    {
        assert(first+count <= 100 && first/64==(first+count-1)/64);
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

// Replaced checkpoints release their old blocks while the mount baseline stays pinned.
static void check_checkpoints(void)
{
    struct block_map map;
    uint64_t original=1, first;
    assert(!map_init(&map, &original, 5));
    map_make_available(&map, 0, 5);
    assert(map_take(&map, 1, &first)==1 && first==1);
    map_release(&map, 0, 1);
    map_checkpoint(&map);
    assert(!map.free_count && map_protected(&map, 0) && map_protected(&map, 1));
    map_make_available(&map, 0, 5);
    map_release(&map, 1, 1);
    assert(map_take(&map, 1, &first)==1 && first==2);
    assert(!map_grow(&map, 130));
    assert(map_protected(&map, 0) && map_protected(&map, 1));
    map_reserve(&map, 128, 1);
    map_checkpoint(&map);
    assert(map_protected(&map, 0) && !map_protected(&map, 1));
    assert(map_protected(&map, 2) && map_protected(&map, 128));
    map_make_available(&map, 0, 5);
    assert(map_take(&map, 1, &first)==1 && first==1);
    map_release(&map, 2, 1);
    map_release(&map, 128, 1);
    map_checkpoint(&map);
    assert(map_protected(&map, 0) && map_protected(&map, 1));
    map_make_available(&map, 0, 5);
    map_make_available(&map, 128, 1);
    assert(map_take(&map, 64, &first)==1 && first==128);
    assert(map_take(&map, 64, &first)==3 && first==2);
    assert(!map_take(&map, 64, &first));
    assert(map.base[0]==1);
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

// Exercise allocator invariants without FUSE or backing storage.
int main(void)
{
    check_snapshots();
    check_checkpoints();
    check_growth();
    check_reuse();
#ifdef TEST_ALLOC_FAILURE
    check_failures();
#endif
    puts("PASS: bitmap baseline/checkpoint protection, bounded runs, growth and recycling");
    return 0;
}
