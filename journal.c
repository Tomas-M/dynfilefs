#define _GNU_SOURCE
#include "journal.h"
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct journal_header
{
    char magic[8];
    uint64_t length, checksum, sequence, size, slots, roots, words, parent;
};

// Append absolute metadata changes only after their referenced data has been synced.
int journal_append(int directory, const char *path, const struct snapshot *snapshot,
                   const uint64_t *previous_bits, size_t previous_words, size_t previous_root_count,
                   const size_t *dirty_roots, size_t dirty_root_count,
                   const size_t *dirty_words, size_t dirty_word_count, int *fd, uint64_t *length)
{
    size_t metadata_length;
    int result=snapshot_length(snapshot, &metadata_length);
    if (result) return result;
    if (!snapshot->size || snapshot->id) return -EINVAL;
    if (!snapshot->sequence || *length%4096 || *length > JOURNAL_LIMIT) return -EINVAL;
    if (snapshot->root_count!=(snapshot->size-1)/(4096*512)+1) return -EINVAL;
    size_t words=(size_t)((snapshot->slots+63)/64), changed=0;
    size_t root_growth=snapshot->root_count > previous_root_count ? snapshot->root_count-previous_root_count : 0;
    size_t word_growth=words > previous_words ? words-previous_words : 0;
    if (root_growth+word_growth > JOURNAL_LIMIT/8) return -E2BIG;
    // The caller lists each touched word once; trimmed zero words need no record entry.
    for (size_t i=0; i < dirty_word_count; i++)
    {
        size_t index=dirty_words[i];
        if (index < words && snapshot->bits[index]!=(index < previous_words ? previous_bits[index] : 0)) changed++;
    }
    size_t maximum=(JOURNAL_LIMIT-sizeof(struct journal_header))/16;
    if (dirty_root_count > maximum || changed > maximum-dirty_root_count) return -E2BIG;
    size_t size=(sizeof(struct journal_header)+(dirty_root_count+changed)*16+4095)&~(size_t)4095;
    if (size > JOURNAL_LIMIT-*length) return -E2BIG;
    unsigned char *bytes=calloc(1, size);
    if (!bytes) return -ENOMEM;
    struct journal_header *header=(struct journal_header *)bytes;
    memcpy(header->magic, "DYNLOG02", 8);
    header->length=htole64(size); header->sequence=htole64(snapshot->sequence);
    header->size=htole64(snapshot->size); header->slots=htole64(snapshot->slots);
    header->roots=htole64(dirty_root_count); header->words=htole64(changed);
    header->parent=htole64(snapshot->parent);
    uint64_t *pairs=(uint64_t *)(bytes+sizeof(*header));
    for (size_t i=0; i < dirty_root_count; i++)
    {
        size_t index=dirty_roots[i];
        if (index >= snapshot->root_count) { free(bytes); return -EINVAL; }
        *pairs++=htole64(index); *pairs++=htole64(snapshot->root[index]);
    }
    for (size_t i=0; i < dirty_word_count; i++)
    {
        size_t index=dirty_words[i];
        if (index < words && snapshot->bits[index]!=(index < previous_words ? previous_bits[index] : 0))
        { *pairs++=htole64(index); *pairs++=htole64(snapshot->bits[index]); }
    }
    header->checksum=htole64(metadata_checksum(bytes, size));
    if (*fd < 0)
    {
        int opened=open_suffix(directory, path, ".log", O_WRONLY|O_CREAT);
        if (opened < 0) { free(bytes); return opened; }
        *fd=opened;
    }
    result=write_at(*fd, bytes, size, (off_t)*length);
    free(bytes);
    if (!result && ftruncate(*fd, (off_t)(*length+size))) result=-errno;
    if (!result && fsync(*fd)) result=-errno;
    if (!result && !*length) result=sync_directory(directory, path);
    if (!result) *length+=size;
    return result;
}

// Bound a record before inspecting its checksum or any counts from disk.
static size_t complete_record(unsigned char *bytes, size_t remaining)
{
    if (remaining < sizeof(struct journal_header)) return 0;
    struct journal_header *header=(struct journal_header *)bytes;
    uint64_t size=le64toh(header->length);
    if (memcmp(header->magic, "DYNLOG02", 8) || size < 4096 || size%4096 || size > remaining) return 0;
    uint64_t saved=header->checksum;
    header->checksum=0;
    uint64_t hash=metadata_checksum(bytes, (size_t)size);
    header->checksum=saved;
    return hash==le64toh(saved) ? (size_t)size : 0;
}

// Validate indices before growing arrays or applying any part of a complete record.
static int apply_record(unsigned char *bytes, size_t length, struct snapshot *snapshot, int apply)
{
    struct journal_header *header=(struct journal_header *)bytes;
    struct snapshot next={.size=le64toh(header->size), .part_size=snapshot->part_size,
                          .slots=le64toh(header->slots), .parent=le64toh(header->parent)};
    if (!next.size) return -EIO;
    size_t metadata_length;
    int result=snapshot_length(&next, &metadata_length);
    if (result) return result;
    uint64_t roots=le64toh(header->roots), words=le64toh(header->words);
    uint64_t maximum=(length-sizeof(*header))/16;
    if (roots > maximum || words > maximum-roots) return -EIO;
    size_t root_count=(size_t)((next.size-1)/(4096*512)+1), word_count=(size_t)((next.slots+63)/64);
    uint64_t *pairs=(uint64_t *)(bytes+sizeof(*header));
    for (uint64_t i=0; i < roots; i++)
        if (le64toh(pairs[i*2]) >= root_count || le64toh(pairs[i*2+1]) > next.slots) return -EIO;
    for (uint64_t i=roots; i < roots+words; i++)
    {
        uint64_t index=le64toh(pairs[i*2]), value=le64toh(pairs[i*2+1]);
        if (index >= word_count) return -EIO;
        if (next.slots%64 && index==word_count-1 && value>>(next.slots%64)) return -EIO;
    }
    if (!apply) return 0;
    if (next.size < snapshot->size) return -EIO;
    size_t previous_words=(size_t)((snapshot->slots+63)/64);
    size_t root_growth=root_count-snapshot->root_count;
    size_t word_growth=word_count > previous_words ? word_count-previous_words : 0;
    if (root_growth+word_growth > JOURNAL_LIMIT/8) return -EIO;
    if (root_count > snapshot->root_count)
    {
        uint64_t *root=realloc(snapshot->root, root_count*8);
        if (!root) return -ENOMEM;
        memset(root+snapshot->root_count, 0, (root_count-snapshot->root_count)*8);
        snapshot->root=root;
    }
    if (word_count > previous_words)
    {
        uint64_t *bits=realloc(snapshot->bits, word_count*8);
        if (!bits) return -ENOMEM;
        memset(bits+previous_words, 0, (word_count-previous_words)*8);
        snapshot->bits=bits;
    }
    for (uint64_t i=0; i < roots; i++) snapshot->root[le64toh(pairs[i*2])]=le64toh(pairs[i*2+1]);
    for (uint64_t i=roots; i < roots+words; i++) snapshot->bits[le64toh(pairs[i*2])]=le64toh(pairs[i*2+1]);
    if (next.slots%64) snapshot->bits[next.slots/64]&=(UINT64_C(1)<<(next.slots%64))-1;
    snapshot->size=next.size; snapshot->slots=next.slots; snapshot->root_count=root_count;
    snapshot->sequence=le64toh(header->sequence);
    snapshot->parent=next.parent;
    return 0;
}

// Recover complete consecutive checkpoints and ignore only a torn final suffix.
int journal_replay(int directory, const char *path, struct snapshot *snapshot, int *replayed)
{
    *replayed=0;
    int fd=open_suffix(directory, path, ".log", O_RDONLY);
    if (fd==-ENOENT) return 0;
    if (fd < 0) return fd;
    struct stat info;
    int result=fstat(fd, &info) ? -errno : 0;
    if (!result && (info.st_size < 0 || (uint64_t)info.st_size > JOURNAL_LIMIT)) result=-EIO;
    size_t length=result ? 0 : (size_t)info.st_size;
    unsigned char *bytes=length ? malloc(length) : NULL;
    if (!result && length && !bytes) result=-ENOMEM;
    if (!result && length) result=read_at(fd, bytes, length, 0);
    close(fd);
    if (result) { free(bytes); return result; }
    uint64_t previous=0;
    for (size_t offset=0; offset < length;)
    {
        size_t size=complete_record(bytes+offset, length-offset);
        if (!size)
        {
            // Later valid metadata proves this is interior damage, not an interrupted append.
            for (size_t later=offset+4096; later < length; later+=4096)
                if (complete_record(bytes+later, length-later)) { result=-EIO; break; }
            break;
        }
        struct journal_header *header=(struct journal_header *)(bytes+offset);
        uint64_t sequence=le64toh(header->sequence);
        if (!sequence || (previous && (previous==UINT64_MAX || sequence!=previous+1))) { result=-EIO; break; }
        int apply=sequence > snapshot->sequence;
        if (apply && (snapshot->sequence==UINT64_MAX || sequence!=snapshot->sequence+1)) { result=-EIO; break; }
        result=apply_record(bytes+offset, size, snapshot, apply);
        if (result) break;
        if (apply) *replayed=1;
        previous=sequence;
        offset+=size;
    }
    free(bytes);
    if (!result) result=snapshot_validate(snapshot);
    return result;
}

// Remove obsolete records only after their replacement is durable.
int journal_reset(int directory, const char *path, int *fd)
{
    // Invalidate before close, and never keep a descriptor to an unlinked journal.
    if (*fd >= 0)
    {
        int previous=*fd;
        *fd=-1;
        if (close(previous)) return -errno;
    }
    char name[PATH_MAX];
    int result=storage_name(name, path, ".log");
    if (!result && unlinkat(directory, name, 0) && errno!=ENOENT) result=-errno;
    if (!result) result=sync_directory(directory, path);
    return result;
}
