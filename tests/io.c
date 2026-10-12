#define _GNU_SOURCE 1
#define FUSE_USE_VERSION 31
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
#undef fuse_main
#define fuse_main(argc, argv, ops, data) test_fuse_main_real(argc, argv, ops, sizeof(*(ops)), data)
#include "../dynfilefs.c"
#undef main
#undef pwrite
#undef fsync
#undef fuse_main

ssize_t write_budget=-1;
int fail_sync=0;
int sync_calls=0;
int stop_writes=0;
int concurrent_writes=0;
int active_writes=0;
int overlapping_writes=0;

struct writer_args
{
    const struct fuse_operations *ops;
    pthread_barrier_t *barrier;
    int worker;
    off_t offset;
};

// Race partial writes to one new block without the kernel's FUSE page lock.
static void *write_partial_block(void *data)
{
    struct writer_args *args=data;
    char bytes[512];
    memset(bytes, args->worker+1, sizeof(bytes));
    pthread_barrier_wait(args->barrier);
    assert(args->ops->write(dynfilefs_path, bytes, sizeof(bytes), args->offset+args->worker*512, NULL)==sizeof(bytes));
    return NULL;
}

// Keep writers busy until fsync has proved it can obtain its barrier.
static void *overwrite_until_sync(void *data)
{
    struct writer_args *args=data;
    char bytes[512];
    memset(bytes, args->worker+1, sizeof(bytes));
    pthread_barrier_wait(args->barrier);
    while (!__atomic_load_n(&stop_writes, __ATOMIC_RELAXED))
        assert(args->ops->write(dynfilefs_path, bytes, sizeof(bytes), args->offset+args->worker*512, NULL)==sizeof(bytes));
    return NULL;
}

// Inject short writes and ENOSPC without replacing the production allocation code.
ssize_t test_pwrite(int fd, const void *buf, size_t size, off_t offset)
{
    if (!write_budget) { errno=ENOSPC; return -1; }
    if (write_budget > 0 && size > write_budget) size=write_budget;
    if (concurrent_writes)
    {
        if (__atomic_add_fetch(&active_writes, 1, __ATOMIC_RELAXED)>1)
            __atomic_store_n(&overlapping_writes, 1, __ATOMIC_RELAXED);
        usleep(10000);
    }
    ssize_t result=pwrite(fd, buf, size, offset);
    if (concurrent_writes) __atomic_sub_fetch(&active_writes, 1, __ATOMIC_RELAXED);
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

    pthread_t threads[8];
    struct writer_args args[8];
    pthread_barrier_t barrier;
    pthread_barrier_init(&barrier, NULL, 8);
    for (int i=0; i < 8; i++)
    {
        args[i]=(struct writer_args){ops, &barrier, i, 3*DATA_BLOCK_SIZE};
        assert(!pthread_create(&threads[i], NULL, write_partial_block, &args[i]));
    }
    for (int i=0; i < 8; i++) pthread_join(threads[i], NULL);
    pthread_barrier_destroy(&barrier);
    assert(ops->read(dynfilefs_path, output, DATA_BLOCK_SIZE, 3*DATA_BLOCK_SIZE, NULL)==DATA_BLOCK_SIZE);
    for (int i=0; i < DATA_BLOCK_SIZE; i++) assert(output[i]==(i<4096 ? i/512+1 : 0));

    fail_sync=1;
    assert(ops->fsync(dynfilefs_path, 0, NULL)==-EIO);
    fail_sync=0;
    sync_calls=0;
    assert(ops->fsync(dynfilefs_path, 0, NULL)==0 && sync_calls);
    sync_calls=0;
    assert(ops->fsync(dynfilefs_path, 0, NULL)==0 && !sync_calls);

    pthread_barrier_init(&barrier, NULL, 9);
    for (int i=0; i < 8; i++)
        assert(!pthread_create(&threads[i], NULL, overwrite_until_sync, &args[i]));
    pthread_barrier_wait(&barrier);
    usleep(10000);
    alarm(5);
    assert(ops->fsync(dynfilefs_path, 0, NULL)==0);
    alarm(0);
    __atomic_store_n(&stop_writes, 1, __ATOMIC_RELAXED);
    for (int i=0; i < 8; i++) pthread_join(threads[i], NULL);
    pthread_barrier_destroy(&barrier);

    // Distinct split files must actually execute their backing writes concurrently.
    pthread_barrier_init(&barrier, NULL, 2);
    concurrent_writes=1;
    args[0].offset=4*DATA_BLOCK_SIZE;
    args[1].offset=split_size-512;
    for (int i=0; i < 2; i++) assert(!pthread_create(&threads[i], NULL, write_partial_block, &args[i]));
    for (int i=0; i < 2; i++) pthread_join(threads[i], NULL);
    concurrent_writes=0;
    assert(overlapping_writes);
    pthread_barrier_destroy(&barrier);

    if (ops->read_buf)
    {
        struct fuse_file_info info={.flags=O_RDONLY|O_DIRECT};
        struct fuse_bufvec *source;
        size_t transfer=3*4096;
        struct fuse_bufvec destination=FUSE_BUFVEC_INIT(transfer);
        destination.buf[0].mem=output;
        assert(ops->read_buf(dynfilefs_path, &source, transfer, 0, &info)==0);
        assert(source->buf[0].flags&FUSE_BUF_IS_FD);
        assert(fuse_buf_copy(&destination, source, 0)==transfer);
        assert(!memcmp(input, output, transfer));
        for (size_t i=0; i < source->count; i++)
            if (!(source->buf[i].flags&FUSE_BUF_IS_FD)) free(source->buf[i].mem);
        free(source);
        info.flags=O_RDONLY;
        assert(ops->read_buf(dynfilefs_path, &source, transfer, 0, &info)==0);
        assert(!(source->buf[0].flags&FUSE_BUF_IS_FD));
        assert(!memcmp(input, source->buf[0].mem, transfer));
        free(source->buf[0].mem);
        free(source);
        info.flags=O_RDONLY|O_DIRECT;
        assert(ops->read_buf(dynfilefs_path, &source, transfer, split_size-512, &info)==0);
        assert(!(source->buf[0].flags&FUSE_BUF_IS_FD));
        destination=FUSE_BUFVEC_INIT(transfer);
        destination.buf[0].mem=output;
        assert(fuse_buf_copy(&destination, source, 0)==transfer);
        for (int i=0; i < transfer; i++) assert(output[i]==(i>=512 && i<1024 ? 2 : 0));
        free(source->buf[0].mem);
        free(source);
        assert(ops->read_buf(dynfilefs_path, &source, 4096, virtual_size, &info)==0);
        assert(fuse_buf_size(source)==0);
        free(source);
    }
    ops->destroy(NULL);
    puts("PASS: ENOSPC, short-write publication, partial-write races, bounds, fsync error/retry/progress, buffer reads");
    return 0;
}

// Reuse normal command-line storage setup, replacing only the FUSE event loop.
int main(int argc, char **argv)
{
    return dynfilefs_main(argc, argv);
}
