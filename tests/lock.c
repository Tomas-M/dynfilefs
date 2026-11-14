#define _GNU_SOURCE
#include "../format.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int pause_ready=-1, pause_resume=-1, fail_sync;

int __real_flock(int fd, int operation);
int __wrap_flock(int fd, int operation);
int __real_fsync(int fd);
int __wrap_fsync(int fd);

// Stop one contender after open but before flock to force the stale-inode race.
int __wrap_flock(int fd, int operation)
{
    if (pause_ready >= 0)
    {
        char byte=1;
        assert(write(pause_ready, &byte, 1)==1);
        assert(read(pause_resume, &byte, 1)==1);
        pause_ready=-1;
    }
    return __real_flock(fd, operation);
}

// Cleanup failures must close the old descriptor without deleting a new owner's lock.
int __wrap_fsync(int fd)
{
    if (fail_sync) { errno=EIO; return -1; }
    return __real_fsync(fd);
}

// A paused old opener must either acquire the replacement inode or reject its owner.
static void check_stale_opener(const char *path, int new_owner)
{
    int owner=lock_storage(AT_FDCWD, path), ready[2], resume[2];
    assert(owner >= 0 && !pipe(ready) && !pipe(resume));
    pid_t child=fork();
    assert(child >= 0);
    if (!child)
    {
        close(owner); close(ready[0]); close(resume[1]);
        pause_ready=ready[1]; pause_resume=resume[0];
        int fd=lock_storage(AT_FDCWD, path);
        if (new_owner) assert(fd==-EWOULDBLOCK);
        else assert(fd >= 0 && !unlock_storage(AT_FDCWD, path, fd));
        _exit(0);
    }
    close(ready[1]); close(resume[0]);
    char byte;
    assert(read(ready[0], &byte, 1)==1 && !unlock_storage(AT_FDCWD, path, owner));
    if (new_owner) { owner=lock_storage(AT_FDCWD, path); assert(owner >= 0); }
    assert(write(resume[1], &byte, 1)==1);
    int status;
    assert(waitpid(child, &status, 0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
    if (new_owner)
    {
        assert(lock_storage(AT_FDCWD, path)==-EWOULDBLOCK);
        assert(!unlock_storage(AT_FDCWD, path, owner));
    }
    close(ready[0]); close(resume[1]);
}

// Exercise ordinary ownership, interrupted owners and deterministic unlink races.
int main(int argc, char **argv)
{
    assert(argc==2);
    char directory[PATH_MAX], path[PATH_MAX], name[PATH_MAX];
    assert(snprintf(directory, sizeof(directory), "%s/lock-XXXXXX", argv[1]) < (int)sizeof(directory));
    assert(mkdtemp(directory));
    assert(snprintf(path, sizeof(path), "%s/store", directory) < (int)sizeof(path));
    assert(!storage_name(name, path, ".lock"));
    alarm(20);
    int owner=lock_storage(AT_FDCWD, path);
    assert(owner >= 0 && lock_storage(AT_FDCWD, path)==-EWOULDBLOCK && !access(name, F_OK));
    close(owner);
    assert(!access(name, F_OK));
    owner=lock_storage(AT_FDCWD, path);
    assert(owner >= 0 && !unlock_storage(AT_FDCWD, path, owner) && access(name, F_OK) && errno==ENOENT);
    check_stale_opener(path, 0);
    check_stale_opener(path, 1);
    owner=lock_storage(AT_FDCWD, path);
    assert(owner >= 0 && !unlink(name));
    int replacement=lock_storage(AT_FDCWD, path);
    assert(replacement >= 0 && unlock_storage(AT_FDCWD, path, owner)==-ESTALE);
    assert(!access(name, F_OK) && lock_storage(AT_FDCWD, path)==-EWOULDBLOCK);
    assert(!unlock_storage(AT_FDCWD, path, replacement));
    owner=lock_storage(AT_FDCWD, path);
    assert(owner >= 0);
    fail_sync=1;
    assert(unlock_storage(AT_FDCWD, path, owner)==-EIO);
    assert(fcntl(owner, F_GETFD)==-1 && errno==EBADF);
    fail_sync=0;
    owner=lock_storage(AT_FDCWD, path);
    assert(owner >= 0 && !unlock_storage(AT_FDCWD, path, owner));
    alarm(0);
    puts("PASS: lock cleanup, interrupted ownership and stale-inode contenders");
    return 0;
}
