#define _GNU_SOURCE
#include "storage.h"
#include "format.h"
#include "bitmap.h"
#include "journal.h"
#include "report.h"
#include <dirent.h>
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
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
struct storage_snapshot
{
    char *name;
    uint64_t id, parent, created, size;
    size_t users;
    struct snapshot state;
    struct storage_snapshot *next;
};
struct storage
{
    char *path;
    int directory_fd, lock_fd, journal_fd, snapshots_fd, changed, finished, directory_dirty, force_snapshot;
    uint64_t size, part_size, capacity, sequence, committed_sequence, log_length, checkpoint_slots, parent, max_id;
    size_t root_count, part_count, dirty_count, tail_count, checkpoint_roots;
    size_t tails[MAX_PARTS];
    uint64_t *root;
    size_t *dirty_leaves;
    struct leaf *leaves;
    struct leaf_cache cache[LOCK_COUNT];
    struct part parts[MAX_PARTS];
    struct block_map map;
    struct storage_snapshot *snapshots;
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
    char prefix[PATH_MAX];
    snprintf(prefix, sizeof(prefix), "%s.", storage->path);
    int fd=openat(storage->directory_fd, ".", O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    if (fd < 0) return -errno;
    DIR *dir=fdopendir(fd);
    if (!dir) { int error=-errno; close(fd); return error; }
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
        int fd=open_suffix(storage->directory_fd, storage->path, suffix, O_RDWR);
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

// Keep UTF-8 names intact while excluding path separators and nonportable control names.
static int snapshot_name(const char *name)
{
    size_t length=strlen(name);
    if (!length || strspn(name, ".")==length || name[length-1]=='.' || name[length-1]==' ') return -EINVAL;
    if (length > DYN_NAME_MAX) return -ENAMETOOLONG;
    for (const unsigned char *p=(const unsigned char *)name; *p; p++)
        if (*p < 32 || *p==127 || strchr("\"*/:<>?\\|", *p)) return -EINVAL;
    return 0;
}

// IDs retain ancestry when a deleted name is reused by a different point.
static struct storage_snapshot *snapshot_id(struct storage *storage, uint64_t id)
{
    for (struct storage_snapshot *entry=storage->snapshots; entry; entry=entry->next)
        if (entry->id==id) return entry;
    return NULL;
}

// Only restorable points occupy user-visible names.
static struct storage_snapshot *snapshot_named(struct storage *storage, const char *name)
{
    for (struct storage_snapshot *entry=storage->snapshots; entry; entry=entry->next)
        if (entry->size && !strcmp(entry->name, name)) return entry;
    return NULL;
}

// Keep metadata in an owned directory whose parent entry is durable before publication.
static int open_snapshots(struct storage *storage, int create)
{
    if (storage->snapshots_fd >= 0) return create ? sync_directory(storage->directory_fd, storage->path) : 0;
    char path[PATH_MAX];
    int result=storage_name(path, storage->path, ".snapshots");
    if (result) return result;
    if (create && mkdirat(storage->directory_fd, path, 0700) && errno!=EEXIST) return -errno;
    int fd=openat(storage->directory_fd, path, O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if (fd < 0) return -errno;
    storage->snapshots_fd=fd;
    return create ? sync_directory(storage->directory_fd, path) : 0;
}

// Decode one saved mapping or history-only node without following metadata symlinks.
static int load_snapshot(struct storage *storage, uint64_t id, struct snapshot *snapshot)
{
    char filename[32];
    snprintf(filename, sizeof(filename), "%016" PRIx64 ".snap", id);
    int fd=openat(storage->snapshots_fd, filename, O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if (fd < 0) return -errno;
    int result=snapshot_load(fd, snapshot);
    close(fd);
    if (!result && (snapshot->id!=id || snapshot->part_size!=storage->part_size)) result=-EIO;
    return result;
}

// Publish both live roots and small deleted ancestors with the same atomic replacement.
static int write_snapshot(struct storage *storage, const struct snapshot *snapshot, int *published)
{
    *published=0;
    int fd=openat(storage->snapshots_fd, ".next", O_RDWR|O_CREAT|O_TRUNC|O_NOFOLLOW|O_CLOEXEC, 0600);
    if (fd < 0) return -errno;
    int result=snapshot_write(fd, snapshot);
    if (close(fd) && !result) result=-errno;
    char filename[32];
    snprintf(filename, sizeof(filename), "%016" PRIx64 ".snap", snapshot->id);
    if (!result && renameat(storage->snapshots_fd, ".next", storage->snapshots_fd, filename)) result=-errno;
    if (result) return result;
    *published=1;
    return fsync(storage->snapshots_fd) ? -errno : 0;
}

// Load catalog metadata and aggregate pins, keeping unopened roots out of resident memory.
static int discover_snapshots(struct storage *storage)
{
    int result=open_snapshots(storage, 0);
    if (result==-ENOENT) return storage->parent ? -EIO : 0;
    if (result) return result;
    // Commit observed deletions before their old data can be reused after a restart.
    if (fsync(storage->snapshots_fd)) return -errno;
    DIR *dir=fdopendir(dup(storage->snapshots_fd));
    if (!dir) return -errno;
    struct dirent *file;
    for (errno=0; (file=readdir(dir)); errno=0)
    {
        size_t length=strlen(file->d_name);
        if (length < 5 || strcmp(file->d_name+length-5, ".snap")) continue;
        char *end, canonical[32];
        errno=0;
        uint64_t id=strtoull(file->d_name, &end, 16);
        snprintf(canonical, sizeof(canonical), "%016" PRIx64 ".snap", id);
        if (errno || !id || end!=file->d_name+16 || strcmp(file->d_name, canonical)) { result=-EIO; break; }
        struct snapshot saved={0};
        result=load_snapshot(storage, id, &saved);
        if (!result) result=snapshot_name(saved.name);
        if (!result && saved.size && snapshot_named(storage, saved.name)) result=-EIO;
        if (!result) result=map_grow(&storage->map, saved.slots);
        struct storage_snapshot *entry=NULL;
        if (!result)
        {
            entry=calloc(1, sizeof(*entry));
            if (!entry || !(entry->name=strdup(saved.name))) result=-ENOMEM;
        }
        if (!result)
        {
            entry->id=id; entry->parent=saved.parent; entry->created=saved.created; entry->size=saved.size;
            struct storage_snapshot **position=&storage->snapshots;
            while (*position && (*position)->id < id) position=&(*position)->next;
            entry->next=*position; *position=entry;
            if (id > storage->max_id) storage->max_id=id;
            for (size_t i=0; i < (saved.slots+63)/64; i++) storage->map.snapshots[i]|=saved.bits[i];
        }
        else { if (entry) free(entry->name); free(entry); }
        snapshot_free(&saved);
        if (result) break;
    }
    if (!result && errno) result=-errno;
    closedir(dir);
    for (struct storage_snapshot *entry=storage->snapshots; !result && entry; entry=entry->next)
        if (entry->parent && !snapshot_id(storage, entry->parent)) result=-EIO;
    if (!result && storage->parent && !snapshot_id(storage, storage->parent)) result=-EIO;
    return result;
}

// Rebuild pins only after previous ambiguous replacements have been made durable.
static int remaining_snapshot_bits(struct storage *storage, struct storage_snapshot *exclude, uint64_t *bits)
{
    if (fsync(storage->snapshots_fd)) return -errno;
    for (struct storage_snapshot *entry=storage->snapshots; entry; entry=entry->next)
    {
        if (entry==exclude || !entry->size) continue;
        struct snapshot saved={0};
        int result=load_snapshot(storage, entry->id, &saved);
        if (!result && saved.slots > storage->map.slots) result=-EIO;
        if (!result)
        {
            entry->size=saved.size;
            for (size_t i=0; i < (saved.slots+63)/64; i++) bits[i]|=saved.bits[i];
        }
        snapshot_free(&saved);
        if (result) return result;
    }
    return 0;
}

// Find the highest live slot in a range using whole bitmap words.
static uint64_t live_end(struct storage *storage, uint64_t first, uint64_t end, int preserve_snapshots)
{
    if (end > storage->map.slots) end=storage->map.slots;
    while (end > first)
    {
        uint64_t word=(end-1)/64, bits=storage->map.used[word];
        if (preserve_snapshots) bits|=storage->map.snapshots[word];
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
            int fd=open_suffix(storage->directory_fd, storage->path, suffix, O_RDWR|O_CREAT|O_EXCL);
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

// Saved points, durable checkpoints and deferred descriptor replies require copy-on-write.
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

// Deferred descriptor replies pin their data until the stopped mount releases all readers.
int storage_read_extent(struct storage *storage, off_t offset, size_t size, int *fd, off_t *position)
{
    if (offset < 0) return -EINVAL;
    if (!size || (uint64_t)offset >= storage->size || size > storage->size-offset) return 0;
    size_t index=offset/(4096*512);
    enter_io(storage);
    pthread_mutex_t *lock=&storage->stripes[leaf_stripe(index)];
    pthread_mutex_lock(lock);
    int result=load_leaf(storage, index, 0);
    if (!result)
    {
        uint64_t ref=storage->leaves[index].page ? storage->leaves[index].page->entries[(offset/4096)%512] : 0;
        if (ref && contiguous_size(storage, index, offset, size, ref)==size)
        {
            *fd=storage->parts[(ref-1)/storage->capacity].fd;
            *position=physical_offset(storage, ref)+offset%4096;
            struct stat st;
            if (fstat(*fd, &st)) result=-errno;
            else result=(uint64_t)*position+size <= (uint64_t)st.st_size;
            if (result > 0)
            {
                pthread_mutex_lock(&storage->allocation);
                map_pin_read(&storage->map, ref-1, (offset%4096+size+4095)/4096);
                pthread_mutex_unlock(&storage->allocation);
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
        int result=sync_directory(storage->directory_fd, storage->path);
        if (result) return result;
    }
    if (storage->changed)
    {
        if (storage->sequence==UINT64_MAX) return -EOVERFLOW;
        uint64_t slots=live_end(storage, 0, storage->map.slots, 0);
        struct snapshot snapshot={.size=storage->size, .part_size=storage->part_size, .slots=slots,
            .root_count=storage->root_count, .root=storage->root, .bits=storage->map.used,
            .sequence=storage->sequence+1, .parent=storage->parent};
        int result=storage->force_snapshot ? -E2BIG : journal_append(storage->directory_fd, storage->path, &snapshot,
            storage->map.checkpoint, (storage->checkpoint_slots+63)/64, storage->checkpoint_roots,
            storage->dirty_leaves, storage->dirty_count, storage->map.dirty_words, storage->map.dirty_count,
            &storage->journal_fd, &storage->log_length);
        if (result==-E2BIG)
        {
            result=snapshot_replace(storage->directory_fd, storage->path, ".work", &snapshot, NULL);
            if (!result) result=journal_reset(storage->directory_fd, storage->path, &storage->journal_fd);
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

// Remove obsolete recovery files only after the current root is durable.
static int discard_checkpoints(struct storage *storage)
{
    const char *path=storage->path;
    if (storage->journal_fd >= 0)
    {
        int fd=storage->journal_fd;
        storage->journal_fd=-1;
        if (close(fd)) return -errno;
    }
    const char *suffixes[]={".work", ".next", ".log", ".migrate"};
    for (size_t i=0; i < sizeof(suffixes)/sizeof(*suffixes); i++)
    {
        char name[PATH_MAX];
        int result=storage_name(name, path, suffixes[i]);
        if (result) return result;
        if (unlinkat(storage->directory_fd, name, 0) && errno!=ENOENT) return -errno;
    }
    return sync_directory(storage->directory_fd, path);
}

// Serialize synchronization against writers while retaining parallel ordinary I/O.
int storage_sync(struct storage *storage)
{
    enter_barrier(storage);
    int result=sync_locked(storage);
    leave_barrier(storage);
    return result;
}

// Publish a saved root, then persist the active lineage before acknowledging creation.
static int create_snapshot_locked(struct storage *storage, const char *name)
{
    if (snapshot_named(storage, name)) return -EEXIST;
    if (storage->max_id==UINT64_MAX) return -EOVERFLOW;
    int result=open_snapshots(storage, 1);
    if (result) return result;
    struct storage_snapshot *entry=calloc(1, sizeof(*entry));
    if (!entry) return -ENOMEM;
    entry->name=strdup(name);
    if (!entry->name) { free(entry); return -ENOMEM; }
    result=sync_locked(storage);
    if (result) { free(entry->name); free(entry); return result; }
    time_t now=time(NULL);
    if (now < 0) { free(entry->name); free(entry); return -EIO; }
    uint64_t slots=live_end(storage, 0, storage->map.slots, 0);
    struct snapshot saved={.size=storage->size, .part_size=storage->part_size, .slots=slots,
        .root_count=storage->root_count, .root=storage->root, .bits=storage->map.used,
        .sequence=storage->sequence, .id=storage->max_id+1, .parent=storage->parent, .created=(uint64_t)now};
    strcpy(saved.name, name);
    int published=0;
    result=write_snapshot(storage, &saved, &published);
    if (!published) { free(entry->name); free(entry); return result; }
    // A published name can survive a failed directory flush and must remain protected.
    entry->id=saved.id; entry->parent=saved.parent; entry->created=saved.created; entry->size=saved.size;
    struct storage_snapshot **position=&storage->snapshots;
    while (*position) position=&(*position)->next;
    *position=entry; storage->max_id=entry->id;
    for (size_t i=0; i < storage->map.words; i++) storage->map.snapshots[i]|=storage->map.used[i];
    if (result) return result;
    storage->parent=entry->id;
    storage->changed=1;
    return sync_locked(storage);
}

// Retain only ancestry metadata after the deletion is durable, then release exclusive slots.
static int delete_snapshot_locked(struct storage *storage, struct storage_snapshot *entry)
{
    if (entry->users) return -EBUSY;
    uint64_t *bits=calloc(storage->map.words ? storage->map.words : 1, 8);
    if (!bits) return -ENOMEM;
    int result=remaining_snapshot_bits(storage, entry, bits);
    if (!result)
    {
        struct snapshot deleted={.part_size=storage->part_size, .id=entry->id,
            .parent=entry->parent, .created=entry->created};
        strcpy(deleted.name, entry->name);
        int published;
        result=write_snapshot(storage, &deleted, &published);
    }
    if (!result)
    {
        entry->size=0;
        map_set_snapshots(&storage->map, bits);
    }
    free(bits);
    return result;
}

// Automatic retention affects only generated points; open images and the active parent stay available.
static int prune_snapshots(struct storage *storage)
{
    size_t count=0;
    for (struct storage_snapshot *entry=storage->snapshots; entry; entry=entry->next)
        if (entry->size && !strncmp(entry->name, "auto-", 5)) count++;
    for (struct storage_snapshot *entry=storage->snapshots; entry && count > 10; entry=entry->next)
    {
        int automatic=entry->size && !strncmp(entry->name, "auto-", 5);
        if (!automatic || entry->users || entry->id==storage->parent) continue;
        int result=delete_snapshot_locked(storage, entry);
        if (result) return result;
        count--;
    }
    return 0;
}

// A clean session boundary reuses its parent's saved state unless mapping or geometry changed.
static int session_snapshot(struct storage *storage)
{
    int result=sync_locked(storage), same=0;
    if (result) return result;
    struct storage_snapshot *parent=snapshot_id(storage, storage->parent);
    if (parent && parent->size)
    {
        struct snapshot saved={0};
        result=load_snapshot(storage, parent->id, &saved);
        uint64_t slots=live_end(storage, 0, storage->map.slots, 0);
        int geometry=saved.size==storage->size && saved.root_count==storage->root_count && saved.slots==slots;
        if (!result && geometry)
        {
            same=!memcmp(saved.root, storage->root, storage->root_count*8);
            if (same && slots) same=!memcmp(saved.bits, storage->map.used, (size_t)((slots+63)/64)*8);
        }
        snapshot_free(&saved);
        if (result) return result;
    }
    if (!same)
    {
        time_t now=time(NULL);
        struct tm utc;
        if (now < 0 || !gmtime_r(&now, &utc)) return -EIO;
        char date[32], name[DYN_NAME_MAX+1];
        if (!strftime(date, sizeof(date), "%Y-%m-%d-%H%M%S", &utc)) return -EOVERFLOW;
        snprintf(name, sizeof(name), "auto-%s-%" PRIu64, date, storage->max_id+1);
        result=create_snapshot_locked(storage, name);
    }
    return result ? result : prune_snapshots(storage);
}

// Explicit names are immutable; the generated prefix is reserved for automatic session points.
int storage_snapshot_create(struct storage *storage, const char *name)
{
    int result=snapshot_name(name);
    if (!result && !strncmp(name, "auto-", 5)) result=-EINVAL;
    if (result) return result;
    enter_barrier(storage);
    result=create_snapshot_locked(storage, name);
    if (!result) result=prune_snapshots(storage);
    leave_barrier(storage);
    return result;
}

// Deleting a point keeps its ID, name and parent so descendants retain their true history.
int storage_snapshot_delete(struct storage *storage, const char *name)
{
    int result=snapshot_name(name);
    if (result) return result;
    enter_barrier(storage);
    struct storage_snapshot *entry=snapshot_named(storage, name);
    result=entry ? delete_snapshot_locked(storage, entry) : -ENOENT;
    leave_barrier(storage);
    return result;
}

// Return one chronological listing of restorable names for the control file and directory.
int storage_snapshot_list(struct storage *storage, char **result)
{
    enter_io(storage);
    size_t length=0;
    for (struct storage_snapshot *entry=storage->snapshots; entry; entry=entry->next)
    {
        if (!entry->size) continue;
        size_t size=strlen(entry->name)+1;
        if (size > INT_MAX-length) { leave_io(storage); return -EOVERFLOW; }
        length+=size;
    }
    char *list=malloc(length+1), *position=list;
    if (!list) { leave_io(storage); return -ENOMEM; }
    for (struct storage_snapshot *entry=storage->snapshots; entry; entry=entry->next)
    {
        if (!entry->size) continue;
        size_t size=strlen(entry->name);
        memcpy(position, entry->name, size); position[size]='\n'; position+=size+1;
    }
    *position=0; *result=list;
    leave_io(storage);
    return (int)length;
}

// Export ancestry independently of open image handles, including deleted structural nodes.
int storage_snapshot_report(struct storage *storage, int json, char **result)
{
    enter_io(storage);
    size_t count=0;
    for (struct storage_snapshot *entry=storage->snapshots; entry; entry=entry->next) count++;
    struct snapshot_record *records=calloc(count ? count : 1, sizeof(*records));
    if (!records) { leave_io(storage); return -ENOMEM; }
    size_t i=0;
    for (struct storage_snapshot *entry=storage->snapshots; entry; entry=entry->next)
        records[i++]=(struct snapshot_record){entry->id, entry->parent, entry->created, entry->size, entry->name};
    int length=snapshot_report(records, count, storage->parent, json, result);
    free(records);
    leave_io(storage);
    return length;
}

// Live reuse capacity includes allocator reservations and lifetime pins unknown to offline inspection.
int storage_get_info(struct storage *storage, struct storage_info *info)
{
    enter_io(storage);
    pthread_mutex_lock(&storage->allocation);
    *info=(struct storage_info){DYN_FORMAT, storage->size, storage->part_size,
        map_reusable_bytes(&storage->map), storage->parent};
    pthread_mutex_unlock(&storage->allocation);
    leave_io(storage);
    return 0;
}

// Named images retain the virtual size recorded at their own creation point.
int storage_snapshot_stat(struct storage *storage, const char *name, uint64_t *size)
{
    int result=snapshot_name(name);
    if (result) return result;
    enter_io(storage);
    struct storage_snapshot *entry=snapshot_named(storage, name);
    if (!entry) result=-ENOENT;
    else *size=entry->size;
    leave_io(storage);
    return result;
}

// Open readers share one decoded immutable root; deletion is blocked until its last close.
int storage_snapshot_open(struct storage *storage, const char *name, struct storage_snapshot **view)
{
    int result=snapshot_name(name);
    if (result) return result;
    enter_barrier(storage);
    struct storage_snapshot *entry=snapshot_named(storage, name);
    if (!entry) result=-ENOENT;
    else
    {
        if (!entry->users) result=load_snapshot(storage, entry->id, &entry->state);
        if (!result && !entry->state.size) { entry->size=0; result=-ENOENT; }
        if (!result) { entry->users++; *view=entry; }
        else snapshot_free(&entry->state);
    }
    leave_barrier(storage);
    return result;
}

// Closing a view does not change the saved point or its persistent block protection.
void storage_snapshot_close(struct storage *storage, struct storage_snapshot *view)
{
    enter_barrier(storage);
    if (!--view->users) snapshot_free(&view->state);
    leave_barrier(storage);
}

// The view remains pinned by the caller until its FUSE or backend handle is released.
uint64_t storage_snapshot_size(struct storage_snapshot *view)
{
    return view->size;
}

// Read the saved mapping independently of the active state's mutable index cache.
int storage_snapshot_read(struct storage *storage, struct storage_snapshot *view, void *buffer, size_t size, off_t offset)
{
    if (offset < 0) return -EINVAL;
    if ((uint64_t)offset >= view->size) return 0;
    if (size > view->size-offset) size=view->size-offset;
    size_t total=0;
    int result=0;
    enter_io(storage);
    while (total < size)
    {
        size_t index=offset/(4096*512), entry=(offset/4096)%512;
        uint64_t leaf[512]={0}, root=view->state.root[index];
        if (root) result=read_physical(storage, root, leaf, sizeof(leaf), 0);
        for (size_t i=0; !result && i < 512; i++)
        {
            leaf[i]=le64toh(leaf[i]);
            if (leaf[i] && (leaf[i] > view->state.slots || !map_test(view->state.bits, leaf[i]-1))) result=-EIO;
        }
        if (result) break;
        uint64_t ref=leaf[entry], part_end=ref ? ((ref-1)/storage->capacity+1)*storage->capacity : 0;
        size_t length=4096-offset%4096;
        if (length > size-total) length=size-total;
        while (length < size-total && ++entry < 512)
        {
            uint64_t next=leaf[entry];
            if ((!ref && next) || (ref && (next!=ref+(offset%4096+length)/4096 || next > part_end))) break;
            size_t remaining=size-total-length;
            length+=remaining > 4096 ? 4096 : remaining;
        }
        if (ref) result=read_physical(storage, ref, buffer, length, offset%4096);
        else memset(buffer, 0, length);
        if (result) break;
        buffer=(char *)buffer+length; offset+=length; total+=length;
    }
    leave_io(storage);
    return result ? result : (int)total;
}

// Publish the current root before discarding the recovery records from which it was built.
static int publish_current(struct storage *storage)
{
    int result=0;
    if (storage->sequence!=storage->committed_sequence)
    {
        uint64_t slots=live_end(storage, 0, storage->map.slots, 0);
        struct snapshot snapshot={.size=storage->size, .part_size=storage->part_size, .slots=slots,
            .root_count=storage->root_count, .root=storage->root, .bits=storage->map.used,
            .sequence=storage->sequence, .parent=storage->parent};
        result=snapshot_publish(storage->directory_fd, storage->path, &snapshot);
        if (!result) storage->committed_sequence=storage->sequence;
    }
    if (!result) result=discard_checkpoints(storage);
    if (!result) storage->log_length=0;
    return result;
}

// Complete the stopped session with one saved point and its durable active parent.
int storage_finish(struct storage *storage)
{
    enter_barrier(storage);
    if (storage->finished) { leave_barrier(storage); return 0; }
    int result=session_snapshot(storage);
    if (!result) result=publish_current(storage);
    if (!result) storage->finished=1;
    leave_barrier(storage);
    return result;
}

// Expose only the current virtual size to the FUSE frontend.
uint64_t storage_size(struct storage *storage)
{
    return storage->size;
}

// Recover current state, select an optional saved branch, and preserve the session's starting point.
int storage_open(struct storage **result, const char *path, uint64_t size, uint64_t increase, uint64_t split, const char *restore)
{
    struct storage *storage=calloc(1, sizeof(*storage));
    if (!storage) return -ENOMEM;
    storage->directory_fd=-1; storage->lock_fd=-1; storage->journal_fd=-1; storage->snapshots_fd=-1;
    for (size_t i=0; i < MAX_PARTS; i++) storage->parts[i].fd=-1;
    pthread_mutex_init(&storage->allocation, NULL);
    pthread_mutex_init(&storage->gate, NULL);
    pthread_cond_init(&storage->wake, NULL);
    for (size_t i=0; i < LOCK_COUNT; i++) pthread_mutex_init(&storage->stripes[i], NULL);
    int error=0;
    struct snapshot snapshot={0};
    error=storage_path(path, &storage->directory_fd, &storage->path);
    if (error) goto failed;
    storage->lock_fd=lock_storage(storage->directory_fd, storage->path);
    if (storage->lock_fd < 0) { error=storage->lock_fd; goto failed; }
    int fd=open_suffix(storage->directory_fd, storage->path, "", O_RDONLY);
    if (fd >= 0)
    {
        error=snapshot_load(fd, &snapshot);
        close(fd);
        if (error==-EPROTONOSUPPORT)
        {
            error=migration_convert(storage->directory_fd, storage->path);
            if (!error)
            {
                fd=open_suffix(storage->directory_fd, storage->path, "", O_RDONLY);
                if (fd < 0) error=fd;
                else { error=snapshot_load(fd, &snapshot); close(fd); }
            }
        }
        if (!error && snapshot.id) error=-EIO;
        if (error) goto failed;
    }
    else if (fd==-ENOENT)
    {
        if (restore) { error=-ENOENT; goto failed; }
        snapshot.size=size ? size : increase;
        snapshot.part_size=split ? split : UINT64_C(4088)*1024*1024;
        if (!snapshot.size || snapshot.size > INT64_MAX || snapshot.size%4096) { error=-EINVAL; goto failed; }
        if (snapshot.part_size/4096 > (UINT64_MAX-1)/MAX_PARTS) { error=-EOVERFLOW; goto failed; }
        uint64_t roots=(snapshot.size-1)/(4096*512)+1;
        if (roots > SIZE_MAX/sizeof(struct leaf)) { error=-EOVERFLOW; goto failed; }
        snapshot.root_count=roots;
        snapshot.root=calloc(snapshot.root_count, 8);
        if (!snapshot.root) { error=-ENOMEM; goto failed; }
        // Orphan data or history cannot be silently adopted by a newly created store.
        storage->part_size=snapshot.part_size;
        error=discover_parts(storage);
        if (error) goto failed;
        if (storage->part_count) { error=-EEXIST; goto failed; }
        const char *pending_suffixes[]={".work", ".log", ".migrate", ".snapshots"};
        for (size_t i=0; i < sizeof(pending_suffixes)/sizeof(*pending_suffixes); i++)
        {
            int pending=open_suffix(storage->directory_fd, storage->path, pending_suffixes[i], O_RDONLY);
            if (pending >= 0) { close(pending); error=-EEXIST; goto failed; }
            if (pending!=-ENOENT) { error=pending; goto failed; }
        }
        error=snapshot_publish(storage->directory_fd, storage->path, &snapshot);
        if (error) goto failed;
        increase=0;
    }
    else { error=fd; goto failed; }
    storage->committed_sequence=snapshot.sequence;
    fd=open_suffix(storage->directory_fd, storage->path, ".work", O_RDONLY);
    if (fd >= 0)
    {
        struct snapshot saved={0};
        error=snapshot_load(fd, &saved);
        close(fd);
        if (!error && (saved.id || saved.part_size!=snapshot.part_size)) error=-EIO;
        if (error) { snapshot_free(&saved); goto failed; }
        if (saved.sequence > snapshot.sequence)
        {
            if (saved.size < snapshot.size) { snapshot_free(&saved); error=-EIO; goto failed; }
            snapshot_free(&snapshot); snapshot=saved;
        }
        else snapshot_free(&saved);
    }
    else if (fd!=-ENOENT) { error=fd; goto failed; }
    int replayed=0;
    error=journal_replay(storage->directory_fd, storage->path, &snapshot, &replayed);
    if (error) goto failed;
    storage->part_size=snapshot.part_size;
    storage->capacity=storage->part_size/4096;
    storage->parent=snapshot.parent;
    if (!storage->capacity || storage->capacity > (UINT64_MAX-1)/MAX_PARTS) { error=-EOVERFLOW; goto failed; }
    error=map_init(&storage->map, snapshot.bits, snapshot.slots);
    if (!error) error=discover_snapshots(storage);
    if (error) goto failed;
    if (restore)
    {
        error=snapshot_name(restore);
        struct storage_snapshot *entry=error ? NULL : snapshot_named(storage, restore);
        if (!error && !entry) error=-ENOENT;
        struct snapshot selected={0};
        if (!error) error=load_snapshot(storage, entry->id, &selected);
        if (!error && snapshot.sequence==UINT64_MAX) error=-EOVERFLOW;
        if (error) { snapshot_free(&selected); goto failed; }
        // Identity belongs to saved nodes; the writable current root records only its chosen parent.
        selected.sequence=snapshot.sequence+1;
        selected.parent=entry->id; selected.id=0; selected.created=0; memset(selected.name, 0, sizeof(selected.name));
        snapshot_free(&snapshot); snapshot=selected;
        storage->parent=snapshot.parent;
        memset(storage->map.used, 0, storage->map.words*8);
        memset(storage->map.checkpoint, 0, storage->map.words*8);
        if (snapshot.slots)
        {
            memcpy(storage->map.used, snapshot.bits, (size_t)((snapshot.slots+63)/64)*8);
            memcpy(storage->map.checkpoint, snapshot.bits, (size_t)((snapshot.slots+63)/64)*8);
        }
    }
    storage->sequence=snapshot.sequence;
    storage->checkpoint_slots=snapshot.slots;
    storage->checkpoint_roots=snapshot.root_count;
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
    error=discover_parts(storage);
    if (!error) error=validate_parts(storage);
    if (error) goto failed;
    // Commit recovery/restore before an automatic point starts a new metadata journal.
    if (storage->sequence!=storage->committed_sequence)
    {
        error=snapshot_publish(storage->directory_fd, storage->path, &snapshot);
        if (error) goto failed;
        storage->committed_sequence=storage->sequence;
    }
    error=discard_checkpoints(storage);
    if (error) goto failed;
    for (size_t i=0; i < storage->part_count; i++)
    {
        uint64_t end=i*storage->capacity+storage->parts[i].blocks;
        error=map_grow(&storage->map, end);
        if (error) goto failed;
    }
    expose_free_blocks(storage);
    for (size_t i=storage->part_count; i-- > 0;)
        if (storage->parts[i].blocks < storage->capacity) storage->tails[storage->tail_count++]=i;
    error=session_snapshot(storage);
    if (!error) error=publish_current(storage);
    if (error) goto failed;
    snapshot_free(&snapshot);
    *result=storage;
    return 0;
failed:
    snapshot_free(&snapshot);
    storage_close(storage);
    return error;
}

// Closing never implicitly commits a failed startup or an interrupted mount.
void storage_close(struct storage *storage)
{
    if (!storage) return;
    if (storage->journal_fd >= 0) close(storage->journal_fd);
    if (storage->snapshots_fd >= 0) close(storage->snapshots_fd);
    while (storage->snapshots)
    {
        struct storage_snapshot *entry=storage->snapshots;
        storage->snapshots=entry->next;
        snapshot_free(&entry->state);
        free(entry->name); free(entry);
    }
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
            int error=unlock_storage(storage->directory_fd, storage->path, storage->lock_fd);
            if (error) fprintf(stderr, "Cannot remove storage lock: %s\n", strerror(-error));
        }
    }
    if (storage->directory_fd >= 0) close(storage->directory_fd);
    free(storage->path);
    pthread_mutex_destroy(&storage->allocation);
    pthread_mutex_destroy(&storage->gate);
    pthread_cond_destroy(&storage->wake);
    for (size_t i=0; i < LOCK_COUNT; i++) pthread_mutex_destroy(&storage->stripes[i]);
    free(storage);
}
