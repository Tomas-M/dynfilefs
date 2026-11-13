#define _GNU_SOURCE
#include "../freeze.c"
#include <assert.h>
#include <stddef.h>
#include <stdarg.h>
#include <sys/mman.h>

struct freeze_counts { int attempts, frozen, thawed, active, fail_at, fail_thaw, stale; };
static struct freeze_counts *counts;

int __wrap_ioctl(int fd, unsigned long request, ...);

// Model kernel freezes across real helper processes without freezing the test host.
int __wrap_ioctl(int fd, unsigned long request, ...)
{
    (void)fd;
    if (request==DYN_LOOP_GET_STATUS64)
    {
        va_list arguments;
        va_start(arguments, request);
        struct loop_status64 *info=va_arg(arguments, struct loop_status64 *);
        memset(info, 0, sizeof(*info));
        info->lo_device=1; info->lo_inode=counts->stale ? 3 : 2;
        va_end(arguments);
        return 0;
    }
    if (request==DYN_FIFREEZE)
    {
        counts->attempts++;
        if (counts->attempts==counts->fail_at) { errno=EBUSY; return -1; }
        counts->frozen++;
        __atomic_add_fetch(&counts->active, 1, __ATOMIC_SEQ_CST);
        return 0;
    }
    assert(request==DYN_FITHAW);
    counts->thawed++;
    if (counts->fail_thaw) { errno=EIO; return -1; }
    __atomic_sub_fetch(&counts->active, 1, __ATOMIC_SEQ_CST);
    return 0;
}

// A killed snapshot caller must not leave its successfully frozen filesystem stuck.
static void check_parent_death(struct freeze_target *targets)
{
    memset(counts, 0, sizeof(*counts));
    int ready[2];
    assert(!pipe(ready));
    pid_t owner=fork();
    assert(owner >= 0);
    if (!owner)
    {
        close(ready[0]);
        struct filesystem_freeze *state=NULL;
        assert(!start_freeze(targets, 1, &state));
        assert(write(ready[1], "1", 1)==1);
        for (;;) pause();
    }
    close(ready[1]);
    char value;
    assert(read(ready[0], &value, 1)==1);
    close(ready[0]);
    assert(counts->active==1);
    assert(!kill(owner, SIGKILL));
    assert(waitpid(owner, NULL, 0)==owner);
    for (int i=0; i < 500 && __atomic_load_n(&counts->active, __ATOMIC_SEQ_CST); i++) usleep(10000);
    assert(!counts->active && counts->thawed==1);
}

// Cover successful freezes, partial failures, pre-existing freezes and lost owners.
int main(void)
{
    assert(sizeof(struct loop_status64)==232 && offsetof(struct loop_status64, lo_init)==216);
    counts=mmap(NULL, sizeof(*counts), PROT_READ|PROT_WRITE, MAP_SHARED|MAP_ANONYMOUS, -1, 0);
    assert(counts!=MAP_FAILED);
    int directory=open("/dev/null", O_RDONLY), loop=open("/dev/null", O_RDONLY);
    assert(directory >= 0 && loop >= 0);
    struct freeze_target targets[]={{directory, loop, 1, 2}, {directory, loop, 1, 2}};
    struct filesystem_freeze *state=NULL;
    assert(!start_freeze(targets, 2, &state) && state);
    assert(counts->active==2 && !counts->thawed);
    assert(!filesystem_thaw(state));
    assert(!counts->active && counts->thawed==2);
    assert(!filesystem_thaw(NULL));

    memset(counts, 0, sizeof(*counts)); counts->fail_at=2; state=NULL;
    assert(start_freeze(targets, 2, &state)==-EBUSY && !state);
    assert(counts->attempts==2 && counts->frozen==1 && counts->thawed==1 && !counts->active);
    memset(counts, 0, sizeof(*counts)); counts->fail_at=1;
    assert(start_freeze(targets, 1, &state)==-EBUSY && !state);
    assert(!counts->frozen && !counts->thawed);
    memset(counts, 0, sizeof(*counts)); counts->stale=1;
    assert(start_freeze(targets, 1, &state)==-ESTALE && !state);
    assert(!counts->attempts);

    memset(counts, 0, sizeof(*counts));
    assert(!start_freeze(targets, 1, &state));
    counts->fail_thaw=1;
    assert(filesystem_thaw(state)==-EIO && counts->thawed==1);
    check_parent_death(targets);

    char decoded[PATH_MAX];
    assert(!mount_path(decoded, "/mnt/žluťoučký\\040kůň\\134test\\011x"));
    assert(!strcmp(decoded, "/mnt/žluťoučký kůň\\test\tx"));
    assert(mount_path(decoded, "/bad\\04")==-EIO);
    assert(mount_path(decoded, "/bad\\00")==-EIO);
    close(directory); close(loop);
    assert(!munmap(counts, sizeof(*counts)));
    puts("PASS: freeze ownership, partial failure, thaw errors, parent death and mount escapes");
    return 0;
}
