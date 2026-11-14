#define _GNU_SOURCE
#include "../storage.h"
#include "../format.h"
#include <assert.h>
#include <dirent.h>
#include <errno.h>
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

static int directory_sync_budget=-1, fail_snapshot_rename, fail_parent_log_sync;
int __real_fsync(int fd);
int __wrap_fsync(int fd);
int __real_renameat(int olddir, const char *oldname, int newdir, const char *newname);
int __wrap_renameat(int olddir, const char *oldname, int newdir, const char *newname);

// Inject failure after publication without failing the preceding data flush.
int __wrap_fsync(int fd)
{
    if (directory_sync_budget >= 0 || fail_parent_log_sync)
    {
        char descriptor[64], path[PATH_MAX];
        snprintf(descriptor, sizeof(descriptor), "/proc/self/fd/%d", fd);
        ssize_t length=readlink(descriptor, path, sizeof(path));
        if (fail_parent_log_sync && length >= 4 && !memcmp(path+length-4, ".log", 4)) { errno=EIO; return -1; }
        if (directory_sync_budget >= 0 && length >= 10 && !memcmp(path+length-10, ".snapshots", 10))
        {
            if (!directory_sync_budget) { errno=EIO; return -1; }
            directory_sync_budget--;
        }
    }
    return __real_fsync(fd);
}

// A failed rename must leave its candidate outside the visible catalog.
int __wrap_renameat(int olddir, const char *oldname, int newdir, const char *newname)
{
    size_t length=strlen(newname);
    if (fail_snapshot_rename && length > 5 && !strcmp(newname+length-5, ".snap")) { errno=EIO; return -1; }
    return __real_renameat(olddir, oldname, newdir, newname);
}

// Restore never supplies a default size that could mask a saved-size regression.
static struct storage *open_store(const char *path, uint64_t size, const char *restore)
{
    struct storage *storage=NULL;
    assert(!storage_open(&storage, path, size, 0, MIB, restore, NULL, 0));
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

// Pin one old mapping while observing its content independently of the active cache.
static void check_saved(struct storage *storage, const char *name, unsigned char value)
{
    struct storage_snapshot *view=NULL;
    assert(!storage_snapshot_open(storage, name, &view));
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
    assert(!storage_snapshot_open(storage, "B", &view));
    assert(storage_snapshot_delete(storage, "B")==-EBUSY);
    storage_snapshot_close(storage, view);
    struct storage_info before, after;
    assert(!storage_get_info(storage, &before));
    uint64_t bytes=part_bytes(path);
    assert(!storage_snapshot_delete(storage, "B"));
    assert(!storage_get_info(storage, &after) && after.reusable >= before.reusable+PAYLOAD_SIZE);
    assert(part_bytes(path)==bytes && storage_snapshot_open(storage, "B", &view)==-ENOENT);
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
    assert(!storage_snapshot_stat(storage, "keep forever", &size) && size==IMAGE_SIZE);
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
    fail_snapshot_rename=1;
    assert(storage_snapshot_create(storage, "unpublished")==-EIO);
    fail_snapshot_rename=0;
    uint64_t size;
    assert(storage_snapshot_stat(storage, "unpublished", &size)==-ENOENT);
    directory_sync_budget=0;
    assert(storage_snapshot_create(storage, "B")==-EIO);
    directory_sync_budget=-1;
    check_saved(storage, "B", 'B');
    write_payload(storage, 'C'); assert(!storage_snapshot_create(storage, "C"));
    struct snapshot saved=named_record(path, "B", 0);
    directory_sync_budget=1;
    assert(storage_snapshot_delete(storage, "B")==-EIO);
    directory_sync_budget=-1;
    write_payload(storage, 'D'); assert(!storage_sync(storage));
    write_payload(storage, 'E'); assert(!storage_sync(storage));
    storage_close(storage);
    // Model an unflushed directory replacement restoring its prior live record after power loss.
    char filename[PATH_MAX];
    assert(snprintf(filename, sizeof(filename), "%s.snapshots/%016" PRIx64 ".snap", path, saved.id) < (int)sizeof(filename));
    int fd=open(filename, O_RDWR|O_TRUNC);
    assert(fd >= 0 && !snapshot_write(fd, &saved));
    close(fd);
    storage=open_store(path, 0, NULL);
    check_saved(storage, "B", 'B'); check_saved(storage, "A", 'A');
    assert(!storage_snapshot_delete(storage, "B"));
    finish_store(storage);
    snapshot_free(&saved);
    puts("PASS: publication failures and ambiguous deleted-record replacement preserve pinned data");
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
    assert(!storage_snapshot_open(storage, "reader", &task.view));
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
    assert(!storage_snapshot_open(storage, "large", &view) && storage_snapshot_size(view)==16*MIB);
    assert(storage_snapshot_read(storage, view, actual, 5, 15*MIB)==5 && !memcmp(actual, "large", 5));
    storage_snapshot_close(storage, view);
    assert(!storage_snapshot_open(storage, "small", &view));
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
    assert(storage_snapshot_list(storage, &names)>0);
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
    assert(!storage_open(&storage, path, 0, 0, MIB, NULL, &first.id, 1));
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
    assert(storage_open(&storage, path, 0, 0, MIB, NULL, &first.id, 1)==-ENOENT && !storage);
    uint64_t missing=UINT64_MAX;
    assert(storage_open(&storage, path, 0, 0, MIB, NULL, &missing, 1)==-ENOENT && !storage);
    snapshot_free(&first);
    puts("PASS: exact mount protection survives recovery/finish pruning and expires only at close");
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
    return 0;
}
