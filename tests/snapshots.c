#define _GNU_SOURCE
#include "../storage.h"
#include "../format.h"
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <endian.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MIB (UINT64_C(1024)*1024)
#define IMAGE_SIZE (8*MIB)
#define PAYLOAD_SIZE (384*1024)

static int fail_intent_write, fail_parent_log_sync;
static const char *fail_canonical_name;
static uint64_t fail_deleted_id;
int __real_fsync(int fd);
ssize_t __real_pwrite(int fd, const void *bytes, size_t count, off_t offset);
ssize_t __real_pwrite64(int fd, const void *bytes, size_t count, off_t offset);

// Identify actual production intent and metadata files without relying on call counts.
static int descriptor_suffix(int fd, const char *suffix)
{
    char descriptor[64], path[PATH_MAX];
    snprintf(descriptor, sizeof(descriptor), "/proc/self/fd/%d", fd);
    ssize_t length=readlink(descriptor, path, sizeof(path)); size_t count=strlen(suffix);
    return length>=(ssize_t)count && !memcmp(path+length-count, suffix, count);
}

// Fail canonical flush only after the requested new record has a complete durable redo.
int __wrap_fsync(int fd)
{
    if (fail_parent_log_sync && descriptor_suffix(fd, ".log")) { errno=EIO; return -1; }
    if ((fail_canonical_name || fail_deleted_id) && descriptor_suffix(fd, ".snap"))
    {
        unsigned char header[4096]; assert(pread(fd, header, sizeof(header), 0)==sizeof(header));
        uint64_t size,id; memcpy(&size,header+16,8); memcpy(&id,header+56,8);
        if ((fail_canonical_name && !strcmp((char *)header+80,fail_canonical_name)) ||
            (fail_deleted_id==le64toh(id) && !le64toh(size))) { errno=EIO; return -1; }
    }
    return __real_fsync(fd);
}

// An unpublished intent must not acquire a visible ID or release old allocator pins.
ssize_t __wrap_pwrite(int fd, const void *bytes, size_t count, off_t offset)
{
    if (fail_intent_write && descriptor_suffix(fd, ".txn")) { errno=EIO; return -1; }
    return __real_pwrite(fd,bytes,count,offset);
}

// Large-file glibc builds redirect pwrite to this symbol.
ssize_t __wrap_pwrite64(int fd, const void *bytes, size_t count, off_t offset)
{
    if (fail_intent_write && descriptor_suffix(fd, ".txn")) { errno=EIO; return -1; }
    return __real_pwrite64(fd,bytes,count,offset);
}

// Restore never supplies a default size that could mask a saved-size regression.
static struct storage *open_store(const char *path, uint64_t size, const char *restore)
{
    struct storage *storage=NULL;
    assert(!storage_open(&storage, path, size, 0, MIB, restore, NULL, 0, 1));
    return storage;
}

// Keep clean shutdown distinct from an interrupted process.
static void finish_store(struct storage *storage)
{
    assert(!storage_finish(storage));
    storage_close(storage);
}

// Compare full generations without overflowing musl's small default pthread stack.
static void check_payload(struct storage *storage, struct storage_snapshot *view, unsigned char value)
{
    unsigned char *actual=malloc(PAYLOAD_SIZE), *expected=malloc(PAYLOAD_SIZE);
    assert(actual && expected);
    memset(expected, value, PAYLOAD_SIZE);
    int result=view ? storage_snapshot_read(storage, view, actual, PAYLOAD_SIZE, 4096) : storage_read(storage, actual, PAYLOAD_SIZE, 4096);
    assert(result==PAYLOAD_SIZE && !memcmp(actual, expected, PAYLOAD_SIZE));
    free(actual); free(expected);
}

// Resolve test display names to canonical identities without adding a production alias API.
static uint64_t point_id(struct storage *storage, const char *name)
{
    char *report=NULL, marker[DYN_NAME_MAX+12]; uint64_t id=0;
    assert(storage_snapshot_report(storage, 1, &report)>0);
    assert(snprintf(marker, sizeof(marker), "\"name\":\"%s\"", name) < (int)sizeof(marker));
    for (char *entry=strstr(report, "{\"id\":\""); entry; entry=strstr(entry+1, "{\"id\":\""))
    {
        char *end=strchr(entry, '}'), *label=strstr(entry, marker), *live=strstr(entry, "\"deleted\":false");
        if (label && label < end && live && live < end) { assert(sscanf(entry+7, "%16" SCNx64, &id)==1); break; }
    }
    free(report); return id;
}

// Pin one old mapping while observing its content independently of the active cache.
static void check_saved(struct storage *storage, const char *name, unsigned char value)
{
    struct storage_snapshot *view=NULL;
    assert(!storage_snapshot_open(storage, point_id(storage, name), &view));
    check_payload(storage, view, value);
    storage_snapshot_close(storage, view);
}

// Every generation overwrites the same logical range, making physical reuse observable.
static void write_payload(struct storage *storage, unsigned char value)
{
    unsigned char *input=malloc(PAYLOAD_SIZE);
    assert(input);
    memset(input, value, PAYLOAD_SIZE);
    assert(storage_write(storage, input, PAYLOAD_SIZE, 4096)==PAYLOAD_SIZE);
    free(input);
}

// Read saved lineage directly from its durable record, including deleted history nodes.
static struct snapshot named_record(const char *path, const char *name, int deleted)
{
    char directory[PATH_MAX], filename[PATH_MAX];
    assert(snprintf(directory, sizeof(directory), "%s.snapshots", path) < (int)sizeof(directory));
    DIR *dir=opendir(directory);
    assert(dir);
    struct snapshot result={0};
    struct dirent *entry;
    while ((entry=readdir(dir)))
    {
        size_t length=strlen(entry->d_name);
        if (length < 5 || strcmp(entry->d_name+length-5, ".snap")) continue;
        assert(snprintf(filename, sizeof(filename), "%s/%s", directory, entry->d_name) < (int)sizeof(filename));
        int fd=open(filename, O_RDONLY);
        struct snapshot saved={0};
        assert(fd >= 0 && !snapshot_load(fd, &saved));
        close(fd);
        if (!strcmp(saved.name, name) && (!saved.size)==deleted)
        {
            assert(!result.id);
            result=saved;
        }
        else snapshot_free(&saved);
    }
    closedir(dir);
    assert(result.id);
    return result;
}

// Count automatic live points separately from their retained history records.
static size_t automatic_points(const char *path)
{
    char directory[PATH_MAX], filename[PATH_MAX];
    assert(snprintf(directory, sizeof(directory), "%s.snapshots", path) < (int)sizeof(directory));
    DIR *dir=opendir(directory);
    assert(dir);
    size_t count=0;
    struct dirent *entry;
    while ((entry=readdir(dir)))
    {
        size_t length=strlen(entry->d_name);
        if (length < 5 || strcmp(entry->d_name+length-5, ".snap")) continue;
        assert(snprintf(filename, sizeof(filename), "%s/%s", directory, entry->d_name) < (int)sizeof(filename));
        int fd=open(filename, O_RDONLY);
        struct snapshot saved={0};
        assert(fd >= 0 && !snapshot_load(fd, &saved));
        close(fd);
        if (saved.size && !strncmp(saved.name, "auto-", 5)) count++;
        snapshot_free(&saved);
    }
    closedir(dir);
    return count;
}

// Sum numbered part lengths; metadata changes must not be mistaken for data growth.
static uint64_t part_bytes(const char *path)
{
    uint64_t result=0;
    for (unsigned i=0; i < DYN_MAX_PARTS; i++)
    {
        char filename[PATH_MAX];
        assert(snprintf(filename, sizeof(filename), "%s.%u", path, i) < (int)sizeof(filename));
        struct stat st;
        if (!stat(filename, &st)) result+=(uint64_t)st.st_size;
        else assert(errno==ENOENT);
    }
    return result;
}

// Restoring B forks the lineage; deleting B retains ancestry and releases its unique blocks.
static void check_lineage(const char *path)
{
    struct storage *storage=open_store(path, IMAGE_SIZE, NULL);
    const char *name="A čistý 系统";
    write_payload(storage, 'A'); assert(!storage_snapshot_create(storage, name));
    write_payload(storage, 'B'); assert(!storage_snapshot_create(storage, "B"));
    write_payload(storage, 'C'); assert(!storage_snapshot_create(storage, "C"));
    struct snapshot a=named_record(path, name, 0), b=named_record(path, "B", 0), c=named_record(path, "C", 0);
    assert(a.id < b.id && b.id < c.id && b.parent==a.id && c.parent==b.id);
    assert(storage_snapshot_create(storage, "B")==-EEXIST);
    assert(storage_snapshot_create(storage, "../escape")==-EINVAL);
    assert(storage_snapshot_create(storage, "auto-user")==-EINVAL);
    finish_store(storage);
    storage=open_store(path, 0, "B");
    check_payload(storage, NULL, 'B');
    write_payload(storage, 'D'); assert(!storage_snapshot_create(storage, "D"));
    struct snapshot d=named_record(path, "D", 0);
    assert(d.parent==b.id);
    check_saved(storage, "C", 'C');
    struct storage_snapshot *view=NULL;
    assert(!storage_snapshot_open(storage, point_id(storage, "B"), &view));
    assert(storage_snapshot_delete(storage, "B")==-EBUSY);
    storage_snapshot_close(storage, view);
    struct storage_info before, after;
    assert(!storage_get_info(storage, &before));
    uint64_t bytes=part_bytes(path);
    assert(!storage_snapshot_delete(storage, "B"));
    assert(!storage_get_info(storage, &after) && after.reusable >= before.reusable+PAYLOAD_SIZE);
    assert(part_bytes(path)==bytes && storage_snapshot_open(storage, point_id(storage, "B"), &view)==-ENOENT);
    struct snapshot deleted=named_record(path, "B", 1);
    assert(deleted.id==b.id && deleted.parent==a.id && !deleted.root && !deleted.bits);
    finish_store(storage);
    storage=open_store(path, 0, name);
    write_payload(storage, 'E'); assert(!storage_snapshot_create(storage, "E"));
    assert(part_bytes(path)==bytes);
    check_saved(storage, "C", 'C'); check_saved(storage, "D", 'D');
    assert(!storage_snapshot_create(storage, "B"));
    struct snapshot renamed=named_record(path, "B", 0);
    assert(renamed.id > d.id && renamed.id!=b.id);
    char *json=NULL;
    assert(storage_snapshot_report(storage, 1, &json) > 0 && strstr(json, "currentParent") && strstr(json, "deleted"));
    free(json);
    finish_store(storage);
    assert(part_bytes(path)==bytes);
    snapshot_free(&a); snapshot_free(&b); snapshot_free(&c); snapshot_free(&d); snapshot_free(&deleted); snapshot_free(&renamed);
    puts("PASS: UTF-8 lineage, restored branches, deleted ancestors, reused names and global block reuse");
}

// Empty sessions reuse their starting point; only automatic payload histories expire.
static void check_sessions(const char *path)
{
    struct storage *storage=open_store(path, IMAGE_SIZE, NULL);
    struct storage_info first, next;
    assert(!storage_get_info(storage, &first) && first.parent && automatic_points(path)==1);
    finish_store(storage);
    storage=open_store(path, 0, NULL);
    assert(!storage_get_info(storage, &next) && next.parent==first.parent && automatic_points(path)==1);
    assert(!storage_snapshot_create(storage, "keep forever"));
    finish_store(storage);
    for (int generation=0; generation < 15; generation++)
    {
        storage=open_store(path, 0, NULL);
        unsigned char value=(unsigned char)(generation+1);
        assert(storage_write(storage, &value, 1, 0)==1);
        finish_store(storage);
        assert(automatic_points(path) <= 10);
        if (generation >= 9) assert(automatic_points(path)==10);
    }
    storage=open_store(path, 0, NULL);
    uint64_t size;
    assert(!storage_snapshot_stat(storage, point_id(storage, "keep forever"), &size) && size==IMAGE_SIZE);
    size_t count=automatic_points(path);
    assert(!storage_get_info(storage, &first));
    finish_store(storage);
    storage=open_store(path, 0, NULL);
    assert(!storage_get_info(storage, &next) && next.parent==first.parent && automatic_points(path)==count);
    finish_store(storage);
    puts("PASS: automatic start/end deduplication, bounded automatic history and permanent manual points");
}

// Both current bytes and their last saved parent must survive journal recovery.
static void check_crash(const char *path)
{
    struct storage *storage=open_store(path, IMAGE_SIZE, NULL);
    write_payload(storage, 'A'); assert(!storage_snapshot_create(storage, "A"));
    struct snapshot a=named_record(path, "A", 0);
    storage_close(storage);
    storage=open_store(path, 0, NULL);
    struct storage_info info;
    assert(!storage_get_info(storage, &info) && info.parent==a.id);
    write_payload(storage, 'B'); assert(!storage_sync(storage));
    storage_close(storage);
    storage=open_store(path, 0, NULL);
    check_payload(storage, NULL, 'B');
    assert(!storage_get_info(storage, &info) && info.parent!=a.id);
    uint64_t recovered=info.parent;
    write_payload(storage, 'C');
    storage_close(storage);
    storage=open_store(path, 0, NULL);
    check_payload(storage, NULL, 'B');
    assert(!storage_get_info(storage, &info) && info.parent==recovered);
    check_saved(storage, "A", 'A');
    finish_store(storage);
    snapshot_free(&a);
    puts("PASS: saved parent and latest fsync survive crashes; unsynced writes create no clean-end point");
}

// Descriptor replies pin old physical bytes until the stopped mount closes.
static void check_read_pins(const char *path)
{
    struct storage *storage=open_store(path, IMAGE_SIZE, NULL);
    write_payload(storage, 'B'); assert(!storage_snapshot_create(storage, "B"));
    int fd;
    off_t offset;
    assert(storage_read_extent(storage, 4096, PAYLOAD_SIZE, &fd, &offset)==1);
    assert(!storage_snapshot_delete(storage, "B"));
    write_payload(storage, 'C'); assert(!storage_sync(storage));
    unsigned char *actual=malloc(PAYLOAD_SIZE);
    assert(actual && pread(fd, actual, PAYLOAD_SIZE, offset)==PAYLOAD_SIZE);
    for (size_t i=0; i < PAYLOAD_SIZE; i++) assert(actual[i]=='B');
    free(actual);
    struct storage_info before, after;
    assert(!storage_get_info(storage, &before));
    finish_store(storage);
    storage=open_store(path, 0, NULL);
    assert(!storage_get_info(storage, &after) && after.reusable >= before.reusable+PAYLOAD_SIZE);
    check_payload(storage, NULL, 'C');
    finish_store(storage);
    puts("PASS: reusableBytes excludes deferred descriptor reads and releases their pins on remount");
}

// Failed publication or deletion must not permit reuse of a potentially durable point.
static void check_faults(const char *path)
{
    struct storage *storage=open_store(path, IMAGE_SIZE, NULL);
    write_payload(storage, 'A'); assert(!storage_snapshot_create(storage, "A"));
    write_payload(storage, 'B');
    fail_intent_write=1;
    assert(storage_snapshot_create(storage, "unpublished")==-EIO);
    fail_intent_write=0;
    uint64_t size;
    assert(storage_snapshot_stat(storage, point_id(storage, "unpublished"), &size)==-ENOENT);
    fail_canonical_name="B";
    assert(storage_snapshot_create(storage, "B")==-EIO);
    fail_canonical_name=NULL;
    check_saved(storage, "B", 'B');
    write_payload(storage, 'C'); assert(!storage_snapshot_create(storage, "C"));
    struct snapshot saved=named_record(path, "B", 0);
    fail_deleted_id=saved.id;
    assert(storage_snapshot_delete(storage, "B")==-EIO);
    fail_deleted_id=0;
    char *unavailable=NULL;
    assert(storage_snapshot_report(storage, 1, &unavailable)>0 && strstr(unavailable, "\"reclaimableBytes\":null"));
    free(unavailable);
    storage_close(storage);
    // Simulate a torn canonical write: the durable deletion intent must repair it.
    char filename[PATH_MAX];
    assert(snprintf(filename, sizeof(filename), "%s.snapshots/%016" PRIx64 ".snap", path, saved.id) < (int)sizeof(filename));
    int fd=open(filename, O_RDWR); assert(fd>=0 && pwrite(fd,"torn",4,0)==4); close(fd);
    storage=open_store(path, 0, NULL);
    check_saved(storage, "A", 'A'); check_saved(storage, "C", 'C'); check_payload(storage, NULL, 'C');
    assert(storage_snapshot_stat(storage,saved.id,&size)==-ENOENT);
    write_payload(storage, 'D'); assert(!storage_snapshot_create(storage,"after recovery"));
    finish_store(storage);
    snapshot_free(&saved);
    puts("PASS: unpublished intents retain pins; committed deletions recover torn canonical records");
}

// A failed parent-only log append remains recoverable after later data changes force a full checkpoint.
static void check_parent_failure(const char *path)
{
    struct storage *storage=open_store(path, IMAGE_SIZE, NULL);
    write_payload(storage, 'A'); assert(!storage_sync(storage));
    fail_parent_log_sync=1;
    assert(storage_snapshot_create(storage, "uncertain")==-EIO);
    fail_parent_log_sync=0;
    struct snapshot saved=named_record(path, "uncertain", 0);
    check_saved(storage, "uncertain", 'A');
    write_payload(storage, 'B'); assert(!storage_sync(storage));
    storage_close(storage);
    storage=open_store(path, 0, NULL);
    check_payload(storage, NULL, 'B'); check_saved(storage, "uncertain", 'A');
    struct storage_info info;
    assert(!storage_get_info(storage, &info));
    char filename[PATH_MAX];
    assert(snprintf(filename, sizeof(filename), "%s.snapshots/%016" PRIx64 ".snap", path, info.parent) < (int)sizeof(filename));
    int fd=open(filename, O_RDONLY);
    struct snapshot current={0};
    assert(fd >= 0 && !snapshot_load(fd, &current));
    close(fd);
    assert(current.parent==saved.id);
    snapshot_free(&current); snapshot_free(&saved);
    finish_store(storage);
    puts("PASS: uncertain parent-only journal append preserves lineage through later full checkpoints");
}

struct reader_task { struct storage *storage; struct storage_snapshot *view; int started; };

// Keep one immutable generation busy during active writes and checkpoints.
static void *read_snapshot(void *argument)
{
    struct reader_task *task=argument;
    __atomic_store_n(&task->started, 1, __ATOMIC_RELEASE);
    for (int i=0; i < 64; i++) check_payload(task->storage, task->view, 'A');
    return NULL;
}

// Users and saved data mappings survive concurrent checkpoint barriers.
static void check_concurrent(const char *path)
{
    struct storage *storage=open_store(path, IMAGE_SIZE, NULL);
    write_payload(storage, 'A'); assert(!storage_snapshot_create(storage, "reader"));
    struct reader_task task={storage, NULL, 0};
    assert(!storage_snapshot_open(storage, point_id(storage, "reader"), &task.view));
    pthread_t reader;
    assert(!pthread_create(&reader, NULL, read_snapshot, &task));
    while (!__atomic_load_n(&task.started, __ATOMIC_ACQUIRE)) sched_yield();
    for (int i=0; i < 32; i++) { write_payload(storage, (unsigned char)('B'+i)); assert(!storage_sync(storage)); }
    assert(storage_snapshot_delete(storage, "reader")==-EBUSY);
    assert(!pthread_join(reader, NULL));
    storage_snapshot_close(storage, task.view);
    assert(!storage_snapshot_delete(storage, "reader"));
    finish_store(storage);
    puts("PASS: immutable concurrent reads and deletion protection during repeated current writes");
}

// Saved sizes and sparse maps remain independent of current disk geometry.
static void check_sparse(const char *path)
{
    unsigned char *expected=calloc(1, IMAGE_SIZE), *actual=malloc(IMAGE_SIZE+37), *input=malloc(MIB+37);
    assert(expected && actual && input);
    for (size_t i=0; i < MIB+37; i++) input[i]=(unsigned char)(1+i*37%251);
    const off_t offsets[]={4095, 2*MIB-17, IMAGE_SIZE-513};
    const size_t sizes[]={8193, MIB+37, 513};
    struct storage *storage=open_store(path, IMAGE_SIZE, NULL);
    for (size_t i=0; i < 3; i++)
    {
        assert(storage_write(storage, input, sizes[i], offsets[i])==(int)sizes[i]);
        memcpy(expected+offsets[i], input, sizes[i]);
    }
    assert(!storage_snapshot_create(storage, "small"));
    finish_store(storage);
    storage=open_store(path, 16*MIB, NULL);
    assert(storage_write(storage, "large", 5, 15*MIB)==5);
    assert(!storage_snapshot_create(storage, "large"));
    finish_store(storage);
    storage=open_store(path, 0, "small");
    assert(storage_size(storage)==IMAGE_SIZE);
    assert(storage_read(storage, actual, IMAGE_SIZE, 0)==IMAGE_SIZE && !memcmp(actual, expected, IMAGE_SIZE));
    struct storage_snapshot *view=NULL;
    assert(!storage_snapshot_open(storage, point_id(storage, "large"), &view) && storage_snapshot_size(view)==16*MIB);
    assert(storage_snapshot_read(storage, view, actual, 5, 15*MIB)==5 && !memcmp(actual, "large", 5));
    storage_snapshot_close(storage, view);
    assert(!storage_snapshot_open(storage, point_id(storage, "small"), &view));
    memset(actual, 0xcc, IMAGE_SIZE+37);
    assert(storage_snapshot_read(storage, view, actual, IMAGE_SIZE+37, 0)==IMAGE_SIZE);
    assert(!memcmp(actual, expected, IMAGE_SIZE));
    for (size_t i=0; i < 37; i++) assert(actual[IMAGE_SIZE+i]==0xcc);
    assert(storage_snapshot_read(storage, view, actual, 1, -1)==-EINVAL);
    storage_snapshot_close(storage, view);
    finish_store(storage);
    free(input); free(actual); free(expected);
    puts("PASS: restored sizes, retained larger views, sparse unaligned maps and EOF bounds");
}

// A queued ID survives recovery pruning before FUSE can open a runtime snapshot handle.
static void check_mount_protection(const char *path)
{
    struct storage *storage=open_store(path, IMAGE_SIZE, NULL);
    char *names=NULL;
    assert(storage_snapshot_list(storage, 0, &names)>0);
    char *newline=strchr(names, '\n');
    if (newline) *newline=0;
    struct snapshot first=named_record(path, names, 0);
    free(names);
    finish_store(storage);
    for (int i=1; i < 10; i++)
    {
        storage=open_store(path, 0, NULL);
        write_payload(storage, (unsigned char)i);
        finish_store(storage);
    }
    assert(automatic_points(path)==10);
    storage=open_store(path, 0, NULL);
    write_payload(storage, 'Z'); assert(!storage_sync(storage));
    storage_close(storage); // Interrupted mount: next open must recover and create point eleven.
    storage=NULL;
    assert(!storage_open(&storage, path, 0, 0, MIB, NULL, &first.id, 1, 1));
    check_payload(storage, NULL, 'Z');
    check_saved(storage, first.name, 0);
    assert(automatic_points(path)==10);
    assert(storage_snapshot_delete(storage, first.name)==-EBUSY);
    struct storage_info current;
    assert(!storage_get_info(storage, &current));
    assert(!storage_snapshot_protect(storage, current.parent, 1));
    assert(storage_snapshot_protect(storage, UINT64_MAX, 1)==-ENOENT);
    assert(!storage_snapshot_create(storage, "Before pending restore"));
    write_payload(storage, 'Y');
    assert(!storage_finish(storage));
    check_saved(storage, first.name, 0);
    assert(!storage_snapshot_protect(storage, current.parent, 0));
    char *catalog=NULL;
    assert(storage_snapshot_report(storage, 1, &catalog)>0 && strstr(catalog, "\"protected\":true"));
    free(catalog);
    storage_close(storage);
    storage=open_store(path, 0, NULL);
    assert(!storage_snapshot_delete(storage, first.name));
    finish_store(storage);
    storage=NULL;
    assert(storage_open(&storage, path, 0, 0, MIB, NULL, &first.id, 1, 1)==-ENOENT && !storage);
    uint64_t missing=UINT64_MAX;
    assert(storage_open(&storage, path, 0, 0, MIB, NULL, &missing, 1, 1)==-ENOENT && !storage);
    snapshot_free(&first);
    puts("PASS: exact mount protection survives recovery/finish pruning and expires only at close");
}

// Read the reported allocator gain independently of internal ownership caches.
static uint64_t reclaimable(struct storage *storage, const char *name)
{
    char *report=NULL, marker[DYN_NAME_MAX+12];
    assert(storage_snapshot_report(storage, 1, &report)>0);
    assert(snprintf(marker, sizeof(marker), "\"name\":\"%s\"", name) < (int)sizeof(marker));
    char *record=strstr(report, marker);
    assert(record);
    char *value=strstr(record, "\"reclaimableBytes\":");
    assert(value && value < strchr(record, '}'));
    value+=strlen("\"reclaimableBytes\":");
    assert(*value >= '0' && *value <= '9');
    uint64_t bytes=strtoull(value, NULL, 10);
    free(report); return bytes;
}

// Shared roots, working writes, crash checkpoints and descriptor pins all exclude false savings.
static void check_reclaimable(const char *path)
{
    struct storage *storage=open_store(path, IMAGE_SIZE, NULL);
    write_payload(storage, 'A'); assert(!storage_snapshot_create(storage, "A"));
    assert(!reclaimable(storage, "A"));
    assert(!storage_snapshot_create(storage, "shared"));
    write_payload(storage, 'B'); assert(!storage_sync(storage));
    assert(!reclaimable(storage, "A") && !reclaimable(storage, "shared"));
    assert(!storage_snapshot_delete(storage, "shared"));
    uint64_t gain=reclaimable(storage, "A");
    assert(gain >= PAYLOAD_SIZE && !reclaimable(storage, "shared"));
    struct storage_snapshot *view=NULL;
    assert(!storage_snapshot_open(storage, point_id(storage, "A"), &view));
    assert(storage_snapshot_delete(storage, "A")==-EBUSY && reclaimable(storage, "A")==gain);
    storage_snapshot_close(storage, view);
    struct storage_info before, after;
    assert(!storage_get_info(storage, &before));
    assert(!storage_snapshot_delete(storage, "A"));
    assert(!storage_get_info(storage, &after) && after.reusable-before.reusable==gain);
    assert(!reclaimable(storage, "A"));
    assert(!storage_snapshot_create(storage, "B"));
    write_payload(storage, 'C');
    assert(!reclaimable(storage, "B"));
    assert(!storage_sync(storage));
    assert(reclaimable(storage, "B") >= PAYLOAD_SIZE);
    assert(!storage_snapshot_create(storage, "pinned"));
    int fd; off_t offset;
    for (size_t i=0; i < PAYLOAD_SIZE; i+=4096)
        assert(storage_read_extent(storage, 4096+i, 4096, &fd, &offset)==1);
    write_payload(storage, 'D'); assert(!storage_sync(storage));
    gain=reclaimable(storage, "pinned");
    assert(gain < PAYLOAD_SIZE);
    assert(!storage_get_info(storage, &before));
    assert(!storage_snapshot_delete(storage, "pinned"));
    assert(!storage_get_info(storage, &after) && after.reusable-before.reusable==gain);
    finish_store(storage);
    puts("PASS: reclaimableBytes matches deletion gains and excludes shared/current/checkpoint/deferred-read blocks");
}

// Disabling automatic points preserves working durability while callers restore their own baseline.
static void check_autosnapshot(const char *path)
{
    struct storage *storage=open_store(path, IMAGE_SIZE, NULL);
    struct storage_info info;
    assert(!storage_get_info(storage, &info) && info.automatic_snapshots==1);
    write_payload(storage, 'A'); assert(!storage_snapshot_create(storage, "baseline"));
    size_t automatic=automatic_points(path);
    assert(!storage_set_autosnapshot(storage, 0));
    assert(!storage_get_info(storage, &info) && !info.automatic_snapshots);
    write_payload(storage, 'B');
    finish_store(storage);
    assert(automatic_points(path)==automatic);
    storage=open_store(path, 0, "baseline");
    check_payload(storage, NULL, 'A');
    assert(!storage_get_info(storage, &info) && info.automatic_snapshots==1);
    assert(automatic_points(path)==automatic);
    assert(!storage_set_autosnapshot(storage, 0));
    write_payload(storage, 'C'); assert(!storage_sync(storage));
    storage_close(storage);
    storage=open_store(path, 0, "baseline");
    check_payload(storage, NULL, 'A');
    assert(automatic_points(path)==automatic);
    assert(!storage_set_autosnapshot(storage, 0));
    write_payload(storage, 'D'); assert(!storage_snapshot_create(storage, "intentional"));
    check_saved(storage, "intentional", 'D');
    assert(!storage_set_autosnapshot(storage, 1));
    write_payload(storage, 'E'); finish_store(storage);
    assert(automatic_points(path)==automatic+1);
    storage=open_store(path, 0, NULL);check_payload(storage, NULL, 'E');finish_store(storage);
    puts("PASS: runtime automatic-point policy, flushed clean end, crash/clean baseline restore and explicit points");
}

// Explicit lifecycle callers can suppress both boundaries and compare live state with its parent.
static void check_mount_snapshot_policy(const char *path)
{
    struct storage *storage=NULL;
    struct storage_info info;
    assert(!storage_open(&storage, path, IMAGE_SIZE, 0, MIB, NULL, NULL, 0, 0));
    assert(!storage_get_info(storage, &info) && !info.automatic_snapshots && !info.matches_parent);
    write_payload(storage, 'A'); assert(!storage_snapshot_create(storage, "baseline"));
    assert(!storage_get_info(storage, &info) && info.matches_parent);
    write_payload(storage, 'B');
    assert(!storage_get_info(storage, &info) && !info.matches_parent);
    assert(!storage_sync(storage));
    assert(!storage_get_info(storage, &info) && !info.matches_parent);
    assert(!storage_snapshot_create(storage, "changed"));
    assert(!storage_get_info(storage, &info) && info.matches_parent);
    finish_store(storage); assert(!automatic_points(path));
    assert(!storage_open(&storage, path, 0, 0, MIB, "baseline", NULL, 0, 0));
    assert(!storage_get_info(storage, &info) && info.matches_parent);
    check_payload(storage, NULL, 'A'); finish_store(storage); assert(!automatic_points(path));
    puts("PASS: mount-time automatic policy and exact parent equality before/after writes and restore");
}

// Names change independently of IDs, ancestry, saved bytes, open readers and protection.
static void check_rename(const char *path)
{
    struct storage *storage=open_store(path, IMAGE_SIZE, NULL);
    struct storage_info info;
    assert(!storage_get_info(storage, &info));
    assert(!storage_snapshot_rename(storage, info.parent, "Kept automatic") && !automatic_points(path));
    write_payload(storage, 'A'); assert(!storage_snapshot_create(storage, "Original"));
    struct snapshot before=named_record(path, "Original", 0);
    assert(!storage_snapshot_create(storage, "Child"));
    struct snapshot child=named_record(path, "Child", 0);
    assert(child.parent==before.id);
    write_payload(storage, 'B'); assert(!storage_sync(storage));
    const char *name="  Český  系统 😀 é Iİı Việt";
    struct storage_snapshot *view=NULL;
    assert(!storage_snapshot_open(storage, point_id(storage, "Original"), &view));
    assert(!storage_snapshot_protect(storage, before.id, 1));
    struct storage_info old_space, new_space;
    assert(!storage_get_info(storage, &old_space));
    assert(!storage_snapshot_rename(storage, before.id, name));
    check_payload(storage, view, 'A'); check_saved(storage, name, 'A');
    assert(!storage_get_info(storage, &new_space) && old_space.reusable==new_space.reusable);
    struct snapshot after=named_record(path, name, 0);
    assert(after.id==before.id && after.parent==before.parent && after.created==before.created && after.size==before.size);
    assert(after.root_count==before.root_count && after.slots==before.slots);
    assert(!memcmp(after.root, before.root, after.root_count*8) && !memcmp(after.bits, before.bits, (size_t)((after.slots+63)/64)*8));
    uint64_t size;
    assert(!storage_snapshot_stat(storage, before.id, &size) && size==IMAGE_SIZE);
    assert(!storage_snapshot_stat(storage, point_id(storage, name), &size) && size==IMAGE_SIZE);
    assert(storage_snapshot_delete(storage, name)==-EBUSY);
    assert(storage_snapshot_rename(storage, before.id, "Child")==-EEXIST);
    assert(storage_snapshot_rename(storage, UINT64_MAX, "Absent")==-ENOENT);
    assert(storage_snapshot_rename(storage, before.id, "auto-user")==-EINVAL);
    assert(storage_snapshot_rename(storage, before.id, "trailing ")==-EINVAL);
    assert(storage_snapshot_rename(storage, before.id, "")==-EINVAL);
    assert(!reclaimable(storage, name));
    assert(!storage_snapshot_delete(storage, "Child"));
    assert(storage_snapshot_rename(storage, child.id, "Deleted")==-ENOENT);
    uint64_t gain=reclaimable(storage, name); assert(gain>=PAYLOAD_SIZE);
    fail_intent_write=1;
    assert(storage_snapshot_rename(storage, before.id, "Not published")==-EIO);
    fail_intent_write=0;
    assert(!storage_snapshot_stat(storage, point_id(storage, name), &size));
    fail_canonical_name="Published";
    assert(storage_snapshot_rename(storage, before.id, "Published")==-EIO);
    fail_canonical_name=NULL;
    assert(!storage_snapshot_stat(storage, before.id, &size));
    assert(!storage_snapshot_stat(storage, point_id(storage, "Published"), &size));
    assert(!storage_snapshot_rename(storage, before.id, "Published"));
    assert(storage_snapshot_create(storage, "Published")==-EEXIST);
    assert(reclaimable(storage, "Published")==gain);
    char full[DYN_NAME_MAX+1];
    for (size_t i=0; i < 124; i++) memcpy(full+i*2, "é", 2);
    memcpy(full+248, "AB", 3);
    assert(!storage_snapshot_rename(storage, before.id, full));
    check_payload(storage, view, 'A'); storage_snapshot_close(storage, view);
    finish_store(storage); storage=open_store(path, 0, full);
    check_payload(storage, NULL, 'A'); check_saved(storage, full, 'A');
    finish_store(storage);
    snapshot_free(&before); snapshot_free(&after); snapshot_free(&child);
    puts("PASS: stable-ID rename preserves Unicode names, content, ancestry and protection; stable open views and publication failures remain coherent");
}

// Every fixture lives in its own directory and remains inspectable on failure.
int main(int argc, char **argv)
{
    if (argc!=2) { fprintf(stderr, "usage: %s temporary-directory\n", argv[0]); return 2; }
    char directory[PATH_MAX], path[PATH_MAX];
    assert(snprintf(directory, sizeof(directory), "%s/snapshots-XXXXXX", argv[1]) < (int)sizeof(directory));
    assert(mkdtemp(directory));
    assert(snprintf(path, sizeof(path), "%s/lineage", directory) < (int)sizeof(path)); check_lineage(path);
    assert(snprintf(path, sizeof(path), "%s/sessions", directory) < (int)sizeof(path)); check_sessions(path);
    assert(snprintf(path, sizeof(path), "%s/crash", directory) < (int)sizeof(path)); check_crash(path);
    assert(snprintf(path, sizeof(path), "%s/read-pins", directory) < (int)sizeof(path)); check_read_pins(path);
    assert(snprintf(path, sizeof(path), "%s/faults", directory) < (int)sizeof(path)); check_faults(path);
    assert(snprintf(path, sizeof(path), "%s/parent-failure", directory) < (int)sizeof(path)); check_parent_failure(path);
    assert(snprintf(path, sizeof(path), "%s/concurrent", directory) < (int)sizeof(path)); check_concurrent(path);
    assert(snprintf(path, sizeof(path), "%s/sparse", directory) < (int)sizeof(path)); check_sparse(path);
    assert(snprintf(path, sizeof(path), "%s/protection", directory) < (int)sizeof(path)); check_mount_protection(path);
    assert(snprintf(path, sizeof(path), "%s/reclaimable", directory) < (int)sizeof(path)); check_reclaimable(path);
    assert(snprintf(path, sizeof(path), "%s/autosnapshot", directory) < (int)sizeof(path)); check_autosnapshot(path);
    assert(snprintf(path, sizeof(path), "%s/mount-policy", directory) < (int)sizeof(path)); check_mount_snapshot_policy(path);
    assert(snprintf(path, sizeof(path), "%s/rename", directory) < (int)sizeof(path)); check_rename(path);
    return 0;
}
