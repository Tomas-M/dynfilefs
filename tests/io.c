#define _GNU_SOURCE 1
#define FUSE_USE_VERSION 26
#include <fuse.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>

ssize_t test_pwrite(int fd, const void *buf, size_t size, off_t offset);
int test_fsync(int fd);
int test_fuse_main_real(int argc, char **argv, const struct fuse_operations *ops, size_t size, void *data);

#define main dynfilefs_main
#define pwrite test_pwrite
#define fsync(fd) test_fsync(fd)
#define fuse_main_real test_fuse_main_real
#include "../dynfilefs.c"
#undef main
#undef pwrite
#undef fsync
#undef fuse_main_real

ssize_t write_budget=-1;
int fail_sync=0;
int sync_calls=0;

// Inject short writes and ENOSPC without replacing the production allocation code.
ssize_t test_pwrite(int fd, const void *buf, size_t size, off_t offset)
{
    if (!write_budget) { errno=ENOSPC; return -1; }
    if (write_budget > 0 && size > write_budget) size=write_budget;
    ssize_t result=pwrite(fd, buf, size, offset);
    if (write_budget > 0 && result > 0) write_budget-=result;
    return result;
}

// A failed synchronization must remain observable and be retried later.
int test_fsync(int fd)
{
    sync_calls++;
    if (fail_sync) { errno=EIO; return -1; }
    return fsync(fd);
}

// Exercise callbacks after the real startup code has created and mapped storage.
int test_fuse_main_real(int argc, char **argv, const struct fuse_operations *ops, size_t size, void *data)
{
    char input[3*DATA_BLOCK_SIZE], output[3*DATA_BLOCK_SIZE];
    memset(input, 'X', sizeof(input));
    write_budget=0;
    assert(ops->write(dynfilefs_path, input, sizeof(input), 0, NULL)==-ENOSPC);
    assert(get_data_offset(0)==0);

    write_budget=DATA_BLOCK_SIZE+17;
    assert(ops->write(dynfilefs_path, input, sizeof(input), 0, NULL)==-ENOSPC);
    assert(get_data_offset(0)!=0 && get_data_offset(DATA_BLOCK_SIZE)!=0);
    assert(get_data_offset(2*DATA_BLOCK_SIZE)==0);
    assert(last_block_offsets[0]==get_data_offset(DATA_BLOCK_SIZE));

    write_budget=-1;
    assert(ops->write(dynfilefs_path, input, sizeof(input), 0, NULL)==sizeof(input));
    assert(ops->read(dynfilefs_path, output, sizeof(output), 0, NULL)==sizeof(output));
    assert(!memcmp(input, output, sizeof(input)));
    assert(ops->write(dynfilefs_path, input, 1, virtual_size, NULL)==-ENOSPC);
    assert(ops->read(dynfilefs_path, output, 1, virtual_size, NULL)==0);
    assert(ops->read(dynfilefs_path, output, 1, -1, NULL)==-EINVAL);

    fail_sync=1;
    assert(ops->fsync(dynfilefs_path, 0, NULL)==-EIO);
    fail_sync=0;
    sync_calls=0;
    assert(ops->fsync(dynfilefs_path, 0, NULL)==0 && sync_calls);

    if (ops->read_buf)
    {
        struct fuse_bufvec *source;
        struct fuse_bufvec destination=FUSE_BUFVEC_INIT(sizeof(output));
        destination.buf[0].mem=output;
        assert(ops->read_buf(dynfilefs_path, &source, sizeof(output), 0, NULL)==0);
        assert(fuse_buf_copy(&destination, source, 0)==sizeof(output));
        assert(!memcmp(input, output, sizeof(input)));
        for (size_t i=0; i < source->count; i++)
            if (!(source->buf[i].flags&FUSE_BUF_IS_FD)) free(source->buf[i].mem);
        free(source);
    }
    ops->destroy(NULL);
    puts("PASS: ENOSPC, short-write publication, bounds, fsync error/retry, buffer reads");
    return 0;
}

// Reuse normal command-line storage setup, replacing only the FUSE event loop.
int main(int argc, char **argv)
{
    return dynfilefs_main(argc, argv);
}
