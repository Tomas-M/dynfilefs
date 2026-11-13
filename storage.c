#define _GNU_SOURCE
#include "storage.h"
#include "format.h"
#include "bitmap.h"
#include "journal.h"
#include <dirent.h>
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_PARTS DYN_MAX_PARTS
#define LOCK_COUNT 256
#if DYN_CACHE_PAGES < LOCK_COUNT || DYN_CACHE_PAGES%LOCK_COUNT
#error DYN_CACHE_PAGES must be a positive multiple of LOCK_COUNT
#endif

struct part { int fd, dirty; uint64_t blocks; };
struct leaf_page
{
    uint64_t entries[512];
    size_t index;
    struct leaf_page *prev, *next;
    int dirty;
};
struct leaf { struct leaf_page *page; int dirty; };
struct leaf_cache { struct leaf_page *first, *last; size_t count; };
struct storage
{
    char *path;
    int lock_fd, journal_fd, rollback, changed, finished, directory_dirty, force_snapshot;
    uint64_t size, part_size, capacity, sequence, base_sequence, log_length, checkpoint_slots;
    size_t root_count, part_count, dirty_count, tail_count, checkpoint_roots;
    size_t tails[MAX_PARTS];
    uint64_t *root;
    size_t *dirty_leaves;
    struct leaf *leaves;
    struct leaf_cache cache[LOCK_COUNT];
    struct part parts[MAX_PARTS];
    struct block_map map;
    pthread_mutex_t allocation, stripes[LOCK_COUNT], gate;
    pthread_cond_t wake;
    size_t readers, waiting;
    int writer;
};

// Waiting synchronization requests stop new I/O from starving the barrier.
static void enter_io(struct storage *storage)
{
    pthread_mutex_lock(&storage->gate);
    while (storage->writer || storage->waiting) pthread_cond_wait(&storage->wake, &storage->gate);
    storage->readers++;
    pthread_mutex_unlock(&storage->gate);
}

// Release one request and wake a waiting synchronizer when it was the last.
static void leave_io(struct storage *storage)
{
    pthread_mutex_lock(&storage->gate);
    if (!--storage->readers) pthread_cond_broadcast(&storage->wake);
    pthread_mutex_unlock(&storage->gate);
}

// Take an exclusive checkpoint or shutdown barrier without serializing data I/O.
static void enter_barrier(struct storage *storage)
{
    pthread_mutex_lock(&storage->gate);
    storage->waiting++;
    while (storage->writer || storage->readers) pthread_cond_wait(&storage->wake, &storage->gate);
    storage->waiting--; storage->writer=1;
    pthread_mutex_unlock(&storage->gate);
}

// Resume requests after metadata and data have reached the same barrier.
static void leave_barrier(struct storage *storage)
{
    pthread_mutex_lock(&storage->gate);
    storage->writer=0;
    pthread_cond_broadcast(&storage->wake);
    pthread_mutex_unlock(&storage->gate);
}

// Resolve relative names before libfuse changes the daemon's working directory.
static char *absolute_path(const char *path)
{
    char name[PATH_MAX];
    if (strlen(path) >= sizeof(name)) { errno=ENAMETOOLONG; return NULL; }
    strcpy(name, path);
    char *slash=strrchr(name, '/'), *base=slash ? slash+1 : name;
    if (!*base) { errno=EINVAL; return NULL; }
    char filename[PATH_MAX];
    strcpy(filename, base);
    if (slash) slash[slash==name ? 1 : 0]=0;
    else strcpy(name, ".");
    char *directory=realpath(name, NULL), *result=NULL;
    if (!directory) return NULL;
    if (asprintf(&result, "%s/%s", directory, filename) < 0) result=NULL;
    free(directory);
    return result;
}

// Physical addresses use fixed-size file windows; files themselves grow lazily.
static off_t physical_offset(struct storage *storage, uint64_t ref)
{
    return ((ref-1)%storage->capacity)*DYN_BLOCK_SIZE;
}

// Mark a file before issuing I/O so failed or partial writes are synchronized too.
static int write_physical(struct storage *storage, uint64_t ref, const void *buffer, size_t size, size_t offset)
{
    struct part *part=&storage->parts[(ref-1)/storage->capacity];
    __atomic_store_n(&part->dirty, 1, __ATOMIC_RELAXED);
    return write_at(part->fd, buffer, size, physical_offset(storage, ref)+offset);
}

// Legacy partial last blocks retain their implicit zero-filled tail.
static int read_physical(struct storage *storage, uint64_t ref, void *buffer, size_t size, size_t offset)
{
    int fd=storage->parts[(ref-1)/storage->capacity].fd;
    off_t position=physical_offset(storage, ref)+offset;
    while (size)
    {
        ssize_t count=pread(fd, buffer, size, position);
        if (count < 0 && errno==EINTR) continue;
        if (count < 0) return -errno;
        if (!count) { memset(buffer, 0, size); return 0; }
        buffer=(char *)buffer+count; position+=count; size-=count;
    }
    return 0;
}

// Discover crash-created parts independently of the committed bitmap's extent.
static int discover_parts(struct storage *storage)
{
    char directory[PATH_MAX];
    strcpy(directory, storage->path);
    char *base=strrchr(directory, '/')+1;
    char prefix[PATH_MAX];
    snprintf(prefix, sizeof(prefix), "%s.", base);
    *base=0;
    DIR *dir=opendir(directory);
    if (!dir) return -errno;
    int result=0;
    struct dirent *entry;
    while ((entry=readdir(dir)))
    {
        if (strncmp(entry->d_name, prefix, strlen(prefix))) continue;
        const char *number=entry->d_name+strlen(prefix);
        if (*number < '0' || *number > '9') continue;
        char *end;
        unsigned long index=strtoul(number, &end, 10);
        if (*end || (number[0]=='0' && number[1])) continue;
        if (index >= MAX_PARTS) { result=-EFBIG; break; }
        char suffix[32];
        snprintf(suffix, sizeof(suffix), ".%lu", index);
        int fd=open_suffix(storage->path, suffix, O_RDWR);
        if (fd < 0) { result=fd; break; }
        struct stat st;
        if (fstat(fd, &st)) { result=-errno; close(fd); break; }
        if (!S_ISREG(st.st_mode) || (uint64_t)st.st_size > storage->part_size)
        { result=-EIO; close(fd); break; }
        storage->parts[index].fd=fd;
        storage->parts[index].blocks=((uint64_t)st.st_size+4095)/4096;
        if (storage->part_count <= index) storage->part_count=index+1;
    }
    closedir(dir);
    return result;
}

// Find the highest live slot in a range using whole bitmap words.
static uint64_t live_end(struct storage *storage, uint64_t first, uint64_t end, int preserve_base)
{
    if (end > storage->map.slots) end=storage->map.slots;
    while (end > first)
    {
        uint64_t word=(end-1)/64, bits=storage->map.used[word];
        if (preserve_base) bits|=storage->map.base[word];
        unsigned upper=(end-1)%64+1;
        if (upper < 64) bits&=(UINT64_C(1)<<upper)-1;
        if (word==first/64) bits&=UINT64_MAX<<(first%64);
        if (bits) return word*64+64-__builtin_clzll(bits);
        end=word*64;
    }
    return first;
}

// Reject missing live blocks before promoting a checkpoint or trimming any file.
static int validate_parts(struct storage *storage)
{
    size_t count=storage->part_count;
    uint64_t referenced=storage->map.slots ? (storage->map.slots-1)/storage->capacity+1 : 0;
    if (referenced > MAX_PARTS) return -EFBIG;
    if (count < referenced) count=referenced;
    for (size_t i=0; i < count; i++)
    {
        uint64_t first=i*storage->capacity;
        uint64_t end=live_end(storage, first, first+storage->capacity, 1);
        if (end > first+storage->parts[i].blocks) return -EIO;
    }
    return 0;
}

// Trim the selected durable state only while no mounted readers remain.
static int trim_parts(struct storage *storage, int preserve_base)
{
    int changed=0;
    for (size_t i=storage->part_count; i-- > 0;)
    {
        struct part *part=&storage->parts[i];
        uint64_t first=i*storage->capacity;
        uint64_t blocks=live_end(storage, first, first+part->blocks, preserve_base)-first;
        if (!blocks)
        {
            if (part->fd >= 0)
            {
                char suffix[32], name[PATH_MAX];
                snprintf(suffix, sizeof(suffix), ".%zu", i);
                storage_name(name, storage->path, suffix);
                if (unlink(name)) return -errno;
                close(part->fd); part->fd=-1; changed=1;
            }
            if (i==storage->part_count-1) storage->part_count--;
            part->blocks=0;
        }
        else if (blocks < part->blocks)
        {
            if (ftruncate(part->fd, blocks*4096) || fsync(part->fd)) return -errno;
            part->blocks=blocks; changed=1;
        }
    }
    return changed ? sync_directory(storage->path) : 0;
}

// Reserve an existing free run, or append at the end of the last physical part.
static int allocate_blocks(struct storage *storage, size_t maximum, uint64_t *ref, size_t *count)
{
    pthread_mutex_lock(&storage->allocation);
    uint64_t first=0;
    size_t length=map_take(&storage->map, maximum, &first);
    int result=0;
    if (length)
    {
        uint64_t remaining=storage->capacity-first%storage->capacity;
        if (length > remaining)
        { map_release(&storage->map, first+remaining, length-remaining); length=remaining; }
    }
    else
    {
        size_t index=storage->tail_count ? storage->tails[storage->tail_count-1] : storage->part_count;
        if (index >= MAX_PARTS) { result=-ENOSPC; goto out; }
        if (!storage->tail_count) storage->tails[storage->tail_count++]=index;
        if (storage->parts[index].fd < 0)
        {
            char suffix[32];
            snprintf(suffix, sizeof(suffix), ".%zu", index);
            int fd=open_suffix(storage->path, suffix, O_RDWR|O_CREAT|O_EXCL);
            if (fd < 0) { result=fd; goto out; }
            storage->parts[index].fd=fd;
            if (storage->part_count <= index) storage->part_count=index+1;
            storage->directory_dirty=1;
        }
        struct part *part=&storage->parts[index];
        uint64_t available=storage->capacity-part->blocks;
        length=available > maximum ? maximum : (size_t)available;
        first=index*storage->capacity+part->blocks;
        result=map_grow(&storage->map, first+length);
        if (result) goto out;
        map_reserve(&storage->map, first, length);
        part->blocks+=length;
        if (storage->tail_count && storage->tails[storage->tail_count-1]==index)
        {
            if (part->blocks==storage->capacity) storage->tail_count--;
        }
    }
    *ref=first+1; *count=length;
out:
    pthread_mutex_unlock(&storage->allocation);
    return result;
}

// Return failed reservations or replaced blocks without releasing baseline pins.
static void release_blocks(struct storage *storage, uint64_t ref, size_t count)
{
    if (!ref) return;
    pthread_mutex_lock(&storage->allocation);
    map_release(&storage->map, ref-1, count);
    pthread_mutex_unlock(&storage->allocation);
}

// Both the mount-start snapshot and last successful checkpoint are immutable.
static int protected_block(struct storage *storage, uint64_t ref)
{
    if (!ref) return 0;
    pthread_mutex_lock(&storage->allocation);
    int protected=map_protected(&storage->map, ref-1);
    pthread_mutex_unlock(&storage->allocation);
    return protected;
}

// Mix nearby and regularly spaced regions across both locks and cache buckets.
static size_t leaf_stripe(size_t index)
{
    return (index^(index>>8)^(index>>16))%LOCK_COUNT;
}

// Move a cached page to the front while its stripe lock excludes other users.
static void touch_leaf(struct leaf_cache *cache, struct leaf_page *page)
{
    if (cache->first==page) return;
    if (page->prev) page->prev->next=page->next;
    if (page->next) page->next->prev=page->prev;
    if (cache->last==page) cache->last=page->prev;
    page->prev=NULL; page->next=cache->first;
    if (cache->first) cache->first->prev=page;
    else cache->last=page;
    cache->first=page;
}

// Spill only the private working leaf; publication still waits for storage_sync().
static int spill_leaf(struct storage *storage, struct leaf_page *page)
{
    if (!page->dirty) return 0;
    uint64_t entries[512];
    for (size_t i=0; i < 512; i++) entries[i]=htole64(page->entries[i]);
    int result=write_physical(storage, storage->root[page->index], entries, sizeof(entries), 0);
    if (!result) page->dirty=0;
    return result;
}

// Bound clean and dirty pages alike, preserving the old cache entry on any I/O failure.
static int load_leaf(struct storage *storage, size_t index, int create)
{
    struct leaf *leaf=&storage->leaves[index];
    struct leaf_cache *cache=&storage->cache[leaf_stripe(index)];
    if (leaf->page) { touch_leaf(cache, leaf->page); return 0; }
    uint64_t ref=storage->root[index];
    if (!ref && !create) return 0;
    uint64_t entries[512]={0};
    int result=0;
    if (ref)
    {
        result=read_at(storage->parts[(ref-1)/storage->capacity].fd, entries, sizeof(entries), physical_offset(storage, ref));
        pthread_mutex_lock(&storage->allocation);
        for (size_t i=0; !result && i < 512; i++)
        {
            entries[i]=le64toh(entries[i]);
            ref=entries[i];
            if (ref && (ref > storage->map.slots || !map_test(storage->map.used, ref-1))) result=-EIO;
        }
        pthread_mutex_unlock(&storage->allocation);
    }
    if (result) return result;
    struct leaf_page *page;
    if (cache->count < DYN_CACHE_PAGES/LOCK_COUNT)
    {
        page=malloc(sizeof(*page));
        if (!page) return -ENOMEM;
        page->prev=NULL; page->next=NULL;
        cache->count++;
    }
    else
    {
        page=cache->last;
        result=spill_leaf(storage, page);
        if (result) return result;
        storage->leaves[page->index].page=NULL;
    }
    memcpy(page->entries, entries, sizeof(entries));
    page->index=index; page->dirty=0;
    leaf->page=page;
    touch_leaf(cache, page);
    return 0;
}

// Copy a protected leaf once per checkpoint and queue modified leaves once.
static int writable_leaf(struct storage *storage, size_t index)
{
    struct leaf *leaf=&storage->leaves[index];
    int result=load_leaf(storage, index, 1);
    if (result) return result;
    uint64_t old=storage->root[index];
    if (!old || protected_block(storage, old))
    {
        uint64_t ref;
        size_t count;
        result=allocate_blocks(storage, 1, &ref, &count);
        if (result) return result;
        storage->root[index]=ref;
        release_blocks(storage, old, 1);
    }
    if (!leaf->dirty)
    {
        pthread_mutex_lock(&storage->allocation);
        storage->dirty_leaves[storage->dirty_count++]=index;
        leaf->dirty=1;
        pthread_mutex_unlock(&storage->allocation);
    }
    leaf->page->dirty=1;
    __atomic_store_n(&storage->changed, 1, __ATOMIC_RELAXED);
    return 0;
}

// Group neighboring holes or consecutive physical addresses within one leaf/file.
static size_t contiguous_size(struct storage *storage, size_t index, off_t offset, size_t size, uint64_t ref)
{
    size_t entry=(offset/4096)%512, length=4096-offset%4096;
    if (length > size) return size;
    uint64_t *entries=storage->leaves[index].page ? storage->leaves[index].page->entries : NULL;
    uint64_t part_end=ref ? ((ref-1)/storage->capacity+1)*storage->capacity : 0;
    while (length < size && ++entry < 512)
    {
        uint64_t next=entries ? entries[entry] : 0;
        if (!ref && next) break;
        if (ref && (next!=ref+(offset%4096+length)/4096 || next > part_end)) break;
        size_t part=size-length;
        length+=part > 4096 ? 4096 : part;
    }
    return length;
}

// Read virtual ranges while keeping their mapping stable until the I/O completes.
int storage_read(struct storage *storage, void *buffer, size_t size, off_t offset)
{
    if (offset < 0) return -EINVAL;
    if ((uint64_t)offset >= storage->size) return 0;
    if (size > storage->size-offset) size=storage->size-offset;
    size_t total=0;
    int result=0;
    enter_io(storage);
    while (total < size)
    {
        size_t index=offset/(4096*512);
        pthread_mutex_t *lock=&storage->stripes[leaf_stripe(index)];
        pthread_mutex_lock(lock);
        result=load_leaf(storage, index, 0);
        uint64_t ref=storage->leaves[index].page ? storage->leaves[index].page->entries[(offset/4096)%512] : 0;
        size_t length=contiguous_size(storage, index, offset, size-total, ref);
        if (!result && ref) result=read_physical(storage, ref, buffer, length, offset%4096);
        if (!result && !ref) memset(buffer, 0, length);
        pthread_mutex_unlock(lock);
        if (result) break;
        buffer=(char *)buffer+length; offset+=length; total+=length;
    }
    leave_io(storage);
    return result ? result : (int)total;
}

// Write whole contiguous runs; only first/last partial COW blocks need old bytes.
int storage_write(struct storage *storage, const void *buffer, size_t size, off_t offset)
{
    static const char zero[4096];
    if (offset < 0) return -EINVAL;
    if ((uint64_t)offset > storage->size || size > storage->size-offset) return -ENOSPC;
    size_t total=0;
    int result=0;
    enter_io(storage);
    while (total < size)
    {
        size_t index=offset/(4096*512), entry=(offset/4096)%512;
        size_t length=4096-offset%4096;
        if (length > size-total) length=size-total;
        pthread_mutex_t *lock=&storage->stripes[leaf_stripe(index)];
        pthread_mutex_lock(lock);
        result=load_leaf(storage, index, 0);
        if (result) goto written;
        uint64_t old=storage->leaves[index].page ? storage->leaves[index].page->entries[entry] : 0;
        if (!old && !memcmp(buffer, zero, length)) goto written;
        int copy=!old || protected_block(storage, old);
        if (copy)
        {
            result=writable_leaf(storage, index);
            if (result) goto written;
        }
        uint64_t *entries=storage->leaves[index].page->entries;
        if (length==4096)
        {
            size_t maximum=(size-total)/4096;
            if (maximum > 512-entry) maximum=512-entry;
            if (maximum > 256) maximum=256;
            size_t blocks=1;
            uint64_t part_end=copy ? 0 : ((old-1)/storage->capacity+1)*storage->capacity;
            pthread_mutex_lock(&storage->allocation);
            for (; blocks < maximum; blocks++)
            {
                uint64_t next=entries[entry+blocks];
                int next_copy=!next || map_protected(&storage->map, next-1);
                if (next_copy!=copy || (!next && !memcmp((const char *)buffer+blocks*4096, zero, 4096))) break;
                if (!copy && (next!=old+blocks || next > part_end)) break;
            }
            pthread_mutex_unlock(&storage->allocation);
            uint64_t target=copy ? 0 : old;
            if (copy) result=allocate_blocks(storage, blocks, &target, &blocks);
            if (!result) result=write_physical(storage, target, buffer, blocks*4096, 0);
            if (copy && result && target) release_blocks(storage, target, blocks);
            if (!result && copy)
            {
                pthread_mutex_lock(&storage->allocation);
                for (size_t i=0; i < blocks; i++)
                {
                    if (entries[entry+i]) map_release(&storage->map, entries[entry+i]-1, 1);
                    entries[entry+i]=target+i;
                }
                pthread_mutex_unlock(&storage->allocation);
            }
            if (!result) length=blocks*4096;
        }
        else if (copy)
        {
            char block[4096]={0};
            uint64_t target=0;
            size_t blocks;
            if (old) result=read_physical(storage, old, block, sizeof(block), 0);
            memcpy(block+offset%4096, buffer, length);
            if (!result) result=allocate_blocks(storage, 1, &target, &blocks);
            if (!result) result=write_physical(storage, target, block, sizeof(block), 0);
            if (result && target) release_blocks(storage, target, 1);
            if (!result) { entries[entry]=target; release_blocks(storage, old, 1); }
        }
        else result=write_physical(storage, old, buffer, length, offset%4096);
written:
        pthread_mutex_unlock(lock);
        if (result) break;
        buffer=(const char *)buffer+length; offset+=length; total+=length;
    }
    leave_io(storage);
    return result ? result : (int)total;
}

// FUSE consumes descriptor replies after this callback; only baseline blocks stay pinned.
int storage_read_extent(struct storage *storage, off_t offset, size_t size, int *fd, off_t *position)
{
    if (offset < 0) return -EINVAL;
    if (!size || (uint64_t)offset >= storage->size || size > storage->size-offset) return 0;
    size_t index=offset/(4096*512);
    int result;
    enter_io(storage);
    pthread_mutex_t *lock=&storage->stripes[leaf_stripe(index)];
    pthread_mutex_lock(lock);
    result=load_leaf(storage, index, 0);
    if (!result)
    {
        uint64_t ref=storage->leaves[index].page ? storage->leaves[index].page->entries[(offset/4096)%512] : 0;
        if (ref && contiguous_size(storage, index, offset, size, ref)==size)
        {
            int pinned=1;
            pthread_mutex_lock(&storage->allocation);
            for (size_t i=0; i < (offset%4096+size+4095)/4096; i++)
                if (!map_test(storage->map.base, ref-1+i)) { pinned=0; break; }
            pthread_mutex_unlock(&storage->allocation);
            if (pinned)
            {
                *fd=storage->parts[(ref-1)/storage->capacity].fd;
                *position=physical_offset(storage, ref)+offset%4096;
                struct stat st;
                if (fstat(*fd, &st)) result=-errno;
                else result=*position+size <= (uint64_t)st.st_size;
            }
        }
    }
    pthread_mutex_unlock(lock);
    leave_io(storage);
    return result;
}

// Seed free-run summaries once, excluding gaps between physical files.
static void expose_free_blocks(struct storage *storage)
{
    for (size_t i=0; i < storage->part_count; i++)
        map_make_available(&storage->map, i*storage->capacity, storage->parts[i].blocks);
}

// Append metadata changes; compact only when the bounded log cannot accept them.
static int sync_locked(struct storage *storage)
{
    for (size_t i=0; i < storage->dirty_count; i++)
    {
        size_t index=storage->dirty_leaves[i];
        struct leaf_page *page=storage->leaves[index].page;
        int result=page ? spill_leaf(storage, page) : 0;
        if (result) return result;
    }
    for (size_t i=0; i < storage->part_count; i++)
        if (storage->parts[i].dirty && fsync(storage->parts[i].fd)) return -errno;
    if (storage->directory_dirty)
    {
        int result=sync_directory(storage->path);
        if (result) return result;
    }
    if (storage->changed)
    {
        if (storage->sequence==UINT64_MAX) return -EOVERFLOW;
        uint64_t slots=live_end(storage, 0, storage->map.slots, 0);
        struct snapshot snapshot={storage->size, storage->part_size, slots, storage->root_count,
                                  storage->root, storage->map.used, storage->sequence+1};
        int result=storage->force_snapshot ? -E2BIG : journal_append(storage->path, &snapshot,
            storage->map.checkpoint, (storage->checkpoint_slots+63)/64, storage->checkpoint_roots,
            storage->dirty_leaves, storage->dirty_count, storage->map.dirty_words, storage->map.dirty_count,
            &storage->journal_fd, &storage->log_length);
        if (result==-E2BIG)
        {
            result=snapshot_replace(storage->path, ".work", &snapshot, NULL);
            if (!result) result=journal_reset(storage->path, &storage->journal_fd);
            if (!result) storage->log_length=0;
        }
        if (result)
        {
            // A failed append/flush can still be recovered; preserve both possible states.
            for (size_t i=0; i < storage->map.words; i++) storage->map.checkpoint[i]|=storage->map.used[i];
            storage->force_snapshot=1;
            return result;
        }
        storage->sequence=snapshot.sequence;
        storage->checkpoint_slots=slots;
        storage->checkpoint_roots=storage->root_count;
        storage->force_snapshot=0;
        map_checkpoint(&storage->map);
        storage->changed=0;
    }
    for (size_t i=0; i < storage->dirty_count; i++) storage->leaves[storage->dirty_leaves[i]].dirty=0;
    for (size_t i=0; i < storage->part_count; i++) storage->parts[i].dirty=0;
    storage->dirty_count=0; storage->directory_dirty=0;
    return 0;
}

// Remove obsolete recovery files only after their replacement or rollback is durable.
static int discard_checkpoints(struct storage *storage, int keep_work)
{
    const char *path=storage->path;
    if (storage->journal_fd >= 0)
    {
        int fd=storage->journal_fd;
        storage->journal_fd=-1;
        if (close(fd)) return -errno;
    }
    const char *suffixes[]={".work", ".next", ".log", ".migrate"};
    for (size_t i=keep_work ? 1 : 0; i < sizeof(suffixes)/sizeof(*suffixes); i++)
    {
        char name[PATH_MAX];
        int result=storage_name(name, path, suffixes[i]);
        if (result) return result;
        if (unlink(name) && errno!=ENOENT) return -errno;
    }
    return sync_directory(path);
}

// Serialize synchronization against writers while retaining parallel ordinary I/O.
int storage_sync(struct storage *storage)
{
    enter_barrier(storage);
    int result=sync_locked(storage);
    leave_barrier(storage);
    return result;
}

// Persist either choice immediately; repeated calls retry any failed directory flush.
int storage_set_rollback(struct storage *storage, int enabled)
{
    enter_barrier(storage);
    int result=0;
    if (enabled)
    {
        int fd=open_suffix(storage->path, ".rollback", O_WRONLY|O_CREAT);
        if (fd < 0) result=fd;
        else
        {
            storage->rollback=1;
            if (fsync(fd)) result=-errno;
            close(fd);
        }
    }
    else
    {
        char name[PATH_MAX];
        result=storage_name(name, storage->path, ".rollback");
        if (!result && unlink(name) && errno!=ENOENT) result=-errno;
        if (!result) storage->rollback=0;
    }
    if (!result) result=sync_directory(storage->path);
    leave_barrier(storage);
    return result;
}

// Read the current request while excluding changes made by the setter.
int storage_get_rollback(struct storage *storage)
{
    enter_io(storage);
    int enabled=storage->rollback;
    leave_io(storage);
    return enabled;
}

// Commit a stopped mount, or retain its pending rollback for the next mount.
int storage_finish(struct storage *storage)
{
    enter_barrier(storage);
    int result=0;
    if (storage->finished) { leave_barrier(storage); return 0; }
    // Preserve cleanly unmounted writes so --norollback can continue this session.
    result=sync_locked(storage);
    if (!result && storage->rollback) result=sync_directory(storage->path);
    else if (!result)
    {
        if (storage->sequence!=storage->base_sequence)
        {
            uint64_t slots=live_end(storage, 0, storage->map.slots, 0);
            struct snapshot snapshot={storage->size, storage->part_size, slots, storage->root_count,
                                      storage->root, storage->map.used, storage->sequence};
            result=snapshot_publish(storage->path, &snapshot);
        }
        if (!result) result=discard_checkpoints(storage, 0);
        if (!result) result=trim_parts(storage, 0);
    }
    if (!result) storage->finished=1;
    leave_barrier(storage);
    return result;
}

// Expose only the current virtual size to the FUSE frontend.
uint64_t storage_size(struct storage *storage)
{
    return storage->size;
}

// Recover the last checkpoint, unless a durable request selects the original root.
int storage_open(struct storage **result, const char *path, uint64_t size, uint64_t increase, uint64_t split, int norollback)
{
    struct storage *storage=calloc(1, sizeof(*storage));
    if (!storage) return -ENOMEM;
    storage->lock_fd=-1;
    storage->journal_fd=-1;
    for (size_t i=0; i < MAX_PARTS; i++) storage->parts[i].fd=-1;
    pthread_mutex_init(&storage->allocation, NULL);
    pthread_mutex_init(&storage->gate, NULL);
    pthread_cond_init(&storage->wake, NULL);
    for (size_t i=0; i < LOCK_COUNT; i++) pthread_mutex_init(&storage->stripes[i], NULL);
    int error=0;
    struct snapshot snapshot={0};
    uint64_t *base_bits=NULL, base_slots=0, base_sequence=0;
    int checkpoint=0;
    storage->path=absolute_path(path);
    if (!storage->path) { error=-errno; goto failed; }
    storage->lock_fd=lock_storage(storage->path);
    if (storage->lock_fd < 0) { error=storage->lock_fd; goto failed; }
    int fd=open_suffix(storage->path, "", O_RDONLY);
    if (fd >= 0)
    {
        error=snapshot_load(fd, &snapshot);
        close(fd);
        if (error==-EPROTONOSUPPORT)
        {
            error=migration_convert(storage->path);
            if (!error)
            {
                fd=open_suffix(storage->path, "", O_RDONLY);
                if (fd < 0) error=fd;
                else { error=snapshot_load(fd, &snapshot); close(fd); }
            }
        }
        if (error) goto failed;
    }
    else if (fd==-ENOENT)
    {
        snapshot.size=size ? size : increase;
        snapshot.part_size=split ? split : UINT64_C(4088)*1024*1024;
        if (!snapshot.size || snapshot.size > INT64_MAX || snapshot.size%4096) { error=-EINVAL; goto failed; }
        if (snapshot.part_size/4096 > (UINT64_MAX-1)/MAX_PARTS) { error=-EOVERFLOW; goto failed; }
        uint64_t roots=(snapshot.size-1)/(4096*512)+1;
        if (roots > SIZE_MAX/sizeof(struct leaf)) { error=-EOVERFLOW; goto failed; }
        snapshot.root_count=roots;
        snapshot.root=calloc(snapshot.root_count, 8);
        if (!snapshot.root) { error=-ENOMEM; goto failed; }
        // Orphan parts without a committed root are not safe to repurpose.
        storage->part_size=snapshot.part_size;
        error=discover_parts(storage);
        if (error) goto failed;
        if (storage->part_count) { error=-EEXIST; goto failed; }
        const char *pending_suffixes[]={".work", ".log", ".migrate"};
        for (size_t i=0; i < sizeof(pending_suffixes)/sizeof(*pending_suffixes); i++)
        {
            int pending=open_suffix(storage->path, pending_suffixes[i], O_RDONLY);
            if (pending >= 0) { close(pending); error=-EEXIST; goto failed; }
            if (pending!=-ENOENT) { error=pending; goto failed; }
        }
        error=snapshot_publish(storage->path, &snapshot);
        if (error) goto failed;
        increase=0;
    }
    else { error=fd; goto failed; }
    int marker=open_suffix(storage->path, ".rollback", O_RDONLY);
    if (marker >= 0) close(marker);
    else if (marker!=-ENOENT) { error=marker; goto failed; }
    int continuing=norollback && marker >= 0;
    if (continuing)
    {
        // Replay changes into C while keeping the original rollback allocation pinned.
        base_slots=snapshot.slots; base_sequence=snapshot.sequence;
        size_t bytes=(size_t)((base_slots+63)/64)*8;
        if (bytes)
        {
            base_bits=malloc(bytes);
            if (!base_bits) { error=-ENOMEM; goto failed; }
            memcpy(base_bits, snapshot.bits, bytes);
        }
    }
    if (marker==-ENOENT || continuing)
    {
        fd=open_suffix(storage->path, ".work", O_RDONLY);
        if (fd >= 0)
        {
            struct snapshot saved={0};
            error=snapshot_load(fd, &saved);
            close(fd);
            if (!error && saved.part_size!=snapshot.part_size) error=-EIO;
            if (error) { snapshot_free(&saved); goto failed; }
            if (saved.sequence > snapshot.sequence)
            {
                if (saved.size < snapshot.size) { snapshot_free(&saved); error=-EIO; goto failed; }
                snapshot_free(&snapshot);
                snapshot=saved;
                checkpoint=1;
            }
            else snapshot_free(&saved);
        }
        else if (fd!=-ENOENT) { error=fd; goto failed; }
        int replayed=0;
        error=journal_replay(storage->path, &snapshot, &replayed);
        if (error) goto failed;
        checkpoint|=replayed;
    }
    storage->sequence=snapshot.sequence;
    storage->base_sequence=continuing ? base_sequence : snapshot.sequence;
    storage->rollback=continuing;
    storage->checkpoint_slots=snapshot.slots;
    storage->checkpoint_roots=snapshot.root_count;
    storage->part_size=snapshot.part_size;
    storage->capacity=storage->part_size/4096;
    if (!storage->capacity || storage->capacity > (UINT64_MAX-1)/MAX_PARTS) { error=-EOVERFLOW; goto failed; }
    if (increase > INT64_MAX-snapshot.size) { error=-EOVERFLOW; goto failed; }
    storage->size=increase ? snapshot.size+increase : (size > snapshot.size ? size : snapshot.size);
    if (storage->size > INT64_MAX || storage->size%4096) { error=-EINVAL; goto failed; }
    uint64_t roots=(storage->size-1)/(4096*512)+1;
    if (roots > SIZE_MAX/sizeof(struct leaf)) { error=-EOVERFLOW; goto failed; }
    storage->root_count=roots;
    storage->root=calloc(storage->root_count, 8);
    storage->leaves=calloc(storage->root_count, sizeof(struct leaf));
    storage->dirty_leaves=malloc(storage->root_count*sizeof(size_t));
    if (!storage->root || !storage->leaves || !storage->dirty_leaves) { error=-ENOMEM; goto failed; }
    memcpy(storage->root, snapshot.root, snapshot.root_count*8);
    storage->changed=storage->size!=snapshot.size;
    error=map_init(&storage->map, snapshot.bits, snapshot.slots);
    if (error) goto failed;
    if (continuing)
    {
        error=map_grow(&storage->map, base_slots);
        if (error) goto failed;
        if (storage->map.words) memset(storage->map.base, 0, storage->map.words*8);
        if (base_slots) memcpy(storage->map.base, base_bits, (size_t)((base_slots+63)/64)*8);
    }
    error=discover_parts(storage);
    if (error) goto failed;
    error=validate_parts(storage);
    if (error) goto failed;
    if (checkpoint)
    {
        error=snapshot_replace(storage->path, continuing ? ".work" : "", &snapshot, NULL);
        if (error) goto failed;
    }
    error=trim_parts(storage, continuing);
    if (error) goto failed;
    for (size_t i=0; i < storage->part_count; i++)
    {
        uint64_t first=i*storage->capacity, end=first+storage->parts[i].blocks;
        error=map_grow(&storage->map, end);
        if (error) goto failed;
    }
    expose_free_blocks(storage);
    // Reuse truncated interior files before adding parts.
    for (size_t i=storage->part_count; i-- > 0;)
        if (storage->parts[i].blocks < storage->capacity) storage->tails[storage->tail_count++]=i;
    // Persist discarded checkpoints before removing the marker that suppresses recovery.
    error=discard_checkpoints(storage, continuing);
    if (error) goto failed;
    if (marker >= 0 && !continuing)
    {
        char name[PATH_MAX];
        storage_name(name, storage->path, ".rollback");
        if (unlink(name)) { error=-errno; goto failed; }
        error=sync_directory(storage->path);
        if (error) goto failed;
    }
    snapshot_free(&snapshot);
    free(base_bits);
    *result=storage;
    return 0;
failed:
    snapshot_free(&snapshot);
    free(base_bits);
    storage_close(storage);
    return error;
}

// Closing never implicitly commits a failed startup or an interrupted mount.
void storage_close(struct storage *storage)
{
    if (!storage) return;
    if (storage->journal_fd >= 0) close(storage->journal_fd);
    for (size_t i=0; i < MAX_PARTS; i++) if (storage->parts[i].fd >= 0) close(storage->parts[i].fd);
    if (storage->leaves)
        for (size_t i=0; i < storage->root_count; i++) free(storage->leaves[i].page);
    free(storage->leaves); free(storage->root); free(storage->dirty_leaves);
    map_destroy(&storage->map);
    if (storage->lock_fd >= 0)
    {
        if (!storage->finished) close(storage->lock_fd);
        else
        {
            int error=unlock_storage(storage->path, storage->lock_fd);
            if (error) fprintf(stderr, "Cannot remove storage lock: %s\n", strerror(-error));
        }
    }
    free(storage->path);
    pthread_mutex_destroy(&storage->allocation);
    pthread_mutex_destroy(&storage->gate);
    pthread_cond_destroy(&storage->wake);
    for (size_t i=0; i < LOCK_COUNT; i++) pthread_mutex_destroy(&storage->stripes[i]);
    free(storage);
}
