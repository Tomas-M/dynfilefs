# Performance checks

Run on Linux as root with libfuse 2.x, Python 3, fio, strace and e2fsprogs installed:

```sh
python3 tests/integration.py /absolute/path/to/dynfilefs
python3 benchmarks/run.py /absolute/path/to/dynfilefs results.json --directory /path/on/test/filesystem --trace
```

The benchmark creates disposable 512 MiB storage split into sixteen 32 MiB parts.
It measures initial allocation, 128 KiB sequential reads and overwrites, eight-worker
4 KiB random reads and overwrites, and 4 KiB writes with an fsync after each write.
Timed workloads run three times for three seconds; the JSON retains all samples
and their medians. The initial allocation is one complete write of the fixture.

Frontend I/O is direct, so reads exercise dynfilefs instead of its FUSE page cache.
Backing-file caching remains enabled. These are warm-cache daemon measurements,
not cold physical-drive benchmarks. Repeat with backing files on ext4 and tmpfs
to distinguish filesystem synchronization costs from CPU and FUSE overhead.
The reported daemon CPU percentage includes all its worker threads; it may exceed 100%.

The optional trace counts storage syscalls during a separate 16 MiB write/read
and is excluded from timed results. The correctness runner covers sparse zeros,
unaligned writes, odd split sizes, split boundaries, EOF, partial physical tails,
remounts, parallel writes, concurrent fsync, loop-mounted ext4, and rejection of the old format.

For the practical XFS loop-device workload, install xfsprogs and run:

```sh
python3 benchmarks/copy.py /absolute/path/to/dynfilefs copy.json --directory /path/on/test/filesystem --source-directory /path/on/tmpfs
```

Each repetition formats fresh 3 GiB storage as XFS, copies one nonzero 1 GiB file,
then copies 1000 files of 16 KiB using eight workers. Both timed phases include
syncfs of the dedicated XFS filesystem. Source files are generated on tmpfs;
large-file copies disable reflinks. SHA-256 checks run after unmounting and
remounting XFS and dynfilefs. The default is three complete repetitions.
