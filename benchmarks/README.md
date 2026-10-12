# Performance checks

Run on Linux as root with libfuse 2.9.x, Python 3.11+, fio, strace and e2fsprogs installed:

```sh
python3 tests/integration.py /absolute/path/to/dynfilefs
python3 benchmarks/run.py /absolute/path/to/dynfilefs results.json --directory /path/on/test/filesystem --trace
```

Add `--baseline /path/to/released/dynfilefs` to the correctness runner to check
format-500 storage compatibility in both directions. The callback-level fault
tests also check ENOSPC, short-write index publication, synchronization retries,
concurrent split-file writes and descriptor-backed reads:

```sh
test_dir=$(mktemp -d)
cc -D_FILE_OFFSET_BITS=64 $(pkg-config --cflags fuse) tests/io.c -o "$test_dir/io" $(pkg-config --libs fuse)
"$test_dir/io" -f "$test_dir/storage" -m "$test_dir/mount" -s 4 -p 3
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
remounting XFS and dynfilefs, comparing file contents rather than image or metadata
bytes. A direct read of the large file is timed after remounting and before hashing.
Backing-file cache remains enabled for this read. The default is three complete repetitions.
Add `--compare /path/to/another/binary` for a paired comparison with identical inputs
and alternating binary order; the option can be repeated.

## Measured results, 2026-10-12

Tests ran in VirtualBox on Debian 13, kernel 7.2.9-slax, with 16 vCPUs
(AMD Ryzen 7 5700) and 23.5 GiB usable RAM. Storage parts were on ext4;
copy inputs were on tmpfs. All versions used the same GCC 14.2.0 toolchain,
musl 1.2.6, libfuse 2.9.9 and UPX 5.2.1, producing static 32-bit i686 binaries.
The baseline is the original 5.00 source at `86b966e`; the final version is `9991a3e`.

The paired XFS comparison used three fresh filesystems per version, identical
input files and alternating binary order. Medians include synchronization for
both copy phases. Read results use warm backing-file cache after remounting.

| XFS workload | Original 5.00 | Optimized | Speed ratio |
| --- | ---: | ---: | ---: |
| Copy 1 GiB and syncfs | 21.45 s | 11.25 s | 1.91x |
| Copy 1000 x 16 KiB, eight workers, and syncfs | 1.103 s | 0.850 s | 1.30x |
| Read 1 GiB after remount | 974 MiB/s | 1055 MiB/s | 1.08x |

Every repetition passed content SHA-256 checks after remounting both filesystems.
File timestamps and other metadata are excluded from these hashes.

The direct-I/O microbenchmarks retained three samples per workload and stage.
The baseline was also rerun at the end; both baseline medians are shown because
the VM's disk timings varied considerably during testing.

| Microbenchmark | Original baseline / rerun | Optimized |
| --- | ---: | ---: |
| ext4 backing, 128 KiB direct read | 883 / 864 MiB/s | 2418 MiB/s |
| tmpfs backing, 128 KiB direct read | 893 / 828 MiB/s | 2373 MiB/s |
| tmpfs backing, 128 KiB overwrite | 1228 / 1167 MiB/s | 1503 MiB/s |
| ext4 backing, 4 KiB write plus fsync | 49.6 / 18.3 IOPS | 159.9 IOPS |
| tmpfs backing, eight-worker 4 KiB random read | 40136 / 37511 IOPS | 34259 IOPS |
| tmpfs backing, eight-worker 4 KiB random overwrite | 24191 / 25612 IOPS | 24663 IOPS |

Large copies, direct reads and synchronization improved. Small random I/O did
not improve consistently; the tmpfs random-read result was lower. These VM results
do not establish cold-drive throughput or a universal speedup for every workload.

Separate traces of a 16 MiB write/read reduced backing writes from 4096 to 128
calls. Direct reads used 256 splice calls instead of 4096 pread calls. A single
msync also occurs during the UPX binary's no-argument startup; runtime index
synchronization no longer calls msync. Fresh storage headers still require an
initial fsync of every part, so this short trace does not measure dirty-part skipping.

The shipped commits batch physically contiguous blocks (`21e41dc`), allow
independent split-file I/O (`966c13f`), synchronize only changed parts (`984ba49`),
and splice large direct reads (`9991a3e`). Per-split mutexes replaced the slower
reader/writer-lock prototype. Splicing incoming writes was discarded because it
slowed small I/O. Splicing buffered reads halved XFS read throughput in the trial;
only large O_DIRECT reads of virtual.dat use descriptor replies in the final code.

Validation covered short writes and ENOSPC, index publication, fsync errors and
retries, concurrent split writes, holes, partial tails, EOF, direct/buffered reads,
loop-mounted ext4 and XFS, and format-500 compatibility in both directions.
Callback tests also passed under AddressSanitizer and UndefinedBehaviorSanitizer.
Format 400 is still rejected, and rebuilding the static binary was byte-identical.

[results.json](results.json) retains the environment, binary hashes, commit IDs,
all microbenchmark samples, paired XFS samples, exploratory single runs and traces.

## FAT32 preallocation and split sizes, 2026-10-12

The FAT32 trial compares the current optimized binary with experimental 16 MiB
and 64 MiB growth reservations, using splits of 256, 1024 and 4088 MiB. Each case
runs three times, with the case order reversed for the second repetition. Every
run creates a fresh regular 8 GiB FAT32 image with 32 KiB clusters, then formats
an 8 GiB virtual.dat as XFS. It copies one nonzero 1 GiB file and 1000 files of
16 KiB with eight workers, including XFS syncfs in both copy timings.

The first 27 runs placed the FAT image on the VM's ext4 disk. Another 27 placed
the image on tmpfs to separate FAT/FUSE costs from VM disk delays. Both matrices
used the same static 32-bit toolchain as the earlier measurements. Here the
baseline is the optimized source at `9991a3e`, rather than the original release.
Read timings use direct reads from XFS with warm backing-file cache. XFS,
dynfilefs and FAT32 are all unmounted and remounted before content verification;
fsck.fat checks the image while the storage files are still present.

Disk-backed medians:

| Split | Reservation | 1 GiB copy + syncfs | 1000 files + syncfs | Read |
| --- | --- | ---: | ---: | ---: |
| 256 MiB | None | 21.03 s | 0.863 s | 776 MiB/s |
| 256 MiB | 16 MiB | 29.96 s | 1.675 s | 767 MiB/s |
| 256 MiB | 64 MiB | 21.11 s | 1.394 s | 751 MiB/s |
| 1024 MiB | None | 12.64 s | 1.457 s | 767 MiB/s |
| 1024 MiB | 16 MiB | 19.94 s | 1.119 s | 721 MiB/s |
| 1024 MiB | 64 MiB | 19.04 s | 1.385 s | 768 MiB/s |
| 4088 MiB | None | 8.86 s | 0.691 s | 785 MiB/s |
| 4088 MiB | 16 MiB | 9.21 s | 0.705 s | 803 MiB/s |
| 4088 MiB | 64 MiB | 9.49 s | 0.618 s | 788 MiB/s |

The 4088 MiB split had the lowest large-copy median, but individual disk-backed
copies varied from 7.44 to 40.49 seconds across the matrix. These results do not
establish a universal split-size advantage or physical USB performance. On the
RAM control, median large-copy times were 3.36, 3.40 and 3.40 seconds without
reservations for the three split sizes. Reservations changed these medians by
approximately -3% to +3%; small-copy and read results also showed no consistent
benefit across split sizes and backing filesystems.

Runtime allocation was recorded in the RAM control before unmounting. With
256 MiB splits, allocated space above the storage parts' file lengths was about
0.8 MiB without reservations, 61 MiB with 16 MiB reservations and 237 MiB with
64 MiB reservations. KEEP_SIZE does not increase the visible file length, but
does reserve disk space while the files are open.

Production preallocation remains disabled. The tested implementation and its
callback fault tests are retained in [preallocation.patch](preallocation.patch)
for further experiments. It reserves only newly appended nonzero blocks and
caps the reservation at the part's data capacity plus its header and index.
For a 4088 MiB part the cap is 4 GiB minus 12 KiB, below FAT32's file-size limit.
The format stays 500, and compatibility with the unpatched binary passed in
both directions.

Unsupported fallocate errors (`EOPNOTSUPP`, `ENOSYS`, `EINVAL`) disable reservations
for that part and allow ordinary writes. Capacity errors (`ENOSPC`, `EDQUOT`,
`EFBIG`) also allow the ordinary write to try; actual I/O errors such as `EIO`
are returned before publishing the new block's index entry. Fault tests passed
for both 32-bit reservation sizes and under 64-bit ASan/UBSan. On a real mounted
128 MiB FAT32 image with approximately 1 MiB free, strace confirmed reservation
`ENOSPC` followed by a successful 64 KiB write. That data passed fsck and content
verification after remounting FAT32 and dynfilefs.

[fat-results.json](fat-results.json) retains all 54 samples, medians, FAT checks,
runtime allocation counts, binary hashes, compiler flags and validation evidence.
Allocation counts were added after the disk matrix and are present only in the
RAM control. All 54 repetitions passed content verification and FAT checks.

To build functional prototypes separately from the repository's production
sources, install libfuse 2 development files, pkg-config and patch, then run from
the repository root. These commands use the system compiler and libraries;
the recorded measurements used the static i686 build described above.

```sh
repo=$PWD
work=$(mktemp -d)
mkdir -p "$work/tests" "$work/mount"
cp dynfilefs.c "$work/"
cp tests/io.c "$work/tests/"
cc -O2 -D_FILE_OFFSET_BITS=64 $(pkg-config --cflags fuse) "$work/dynfilefs.c" -o "$work/none" $(pkg-config --libs fuse)
patch -d "$work" -p1 < "$repo/benchmarks/preallocation.patch"
for reserve in 16 64; do
    flags="-O2 -D_FILE_OFFSET_BITS=64 -DPREALLOCATE_SIZE=$((reserve*1024*1024))"
    cc $flags $(pkg-config --cflags fuse) "$work/dynfilefs.c" -o "$work/reserve$reserve" $(pkg-config --libs fuse)
    cc $flags $(pkg-config --cflags fuse) "$work/tests/io.c" -o "$work/io$reserve" $(pkg-config --libs fuse)
    "$work/io$reserve" -f "$work/test$reserve" -m "$work/mount" -s 4 -p 3
done
```

With root access, dosfstools, xfsprogs, e2fsprogs and strace installed:

```sh
python3 -m benchmarks.fat fat.json --binary none="$work/none" --binary reserve16="$work/reserve16" --binary reserve64="$work/reserve64" --directory /path/on/test/filesystem --source-directory /path/on/tmpfs
python3 tests/fat.py "$work/reserve64" --baseline "$work/none" --directory /path/on/test/filesystem --output fat-validation.json
```

The runners format only fresh temporary regular images, never a block device.
Repeat the matrix with an image directory on tmpfs for the RAM control, or on
the actual target USB filesystem to measure that device's behavior.
