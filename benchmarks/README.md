# Performance checks

Run on Linux as root with libfuse 3.8+, Python 3.11+, fio, strace and e2fsprogs installed:

```sh
python3 tests/integration.py /absolute/path/to/dynfilefs
python3 benchmarks/run.py /absolute/path/to/dynfilefs results.json --directory /path/on/test/filesystem --trace
```

Add `--baseline /path/to/released/dynfilefs` to the correctness runner to check
storage compatibility between builds of the same format in both directions. The callback-level fault
tests also check ENOSPC, short-write index publication, synchronization retries,
concurrent split-file writes and descriptor-backed reads:

```sh
test_dir=$(mktemp -d)
cc -D_FILE_OFFSET_BITS=64 $(pkg-config --cflags fuse3) tests/io.c -o "$test_dir/io" $(pkg-config --libs fuse3)
"$test_dir/io" -f "$test_dir/storage" -m "$test_dir/mount" -s 4 -p 3
```

The benchmark creates disposable 512 MiB storage split into sixteen 32 MiB parts.
It measures initial allocation, 128 KiB and 1 MiB sequential reads and overwrites, eight-worker
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

For a comparison of FUSE and allocation-block sizes on FAT32, install dosfstools
and run the following from the repository root:

```sh
python3 -m benchmarks.transfer transfer.json --directory /path/on/ext4 --source-directory /path/on/tmpfs --binary fuse2-4k=/path/to/baseline --binary fuse3-4k=/path/to/intermediate --binary fuse3-512k=/path/to/current
```

Each case formats its own disposable 8 GiB regular image as FAT32 with 32 KiB
clusters. The XFS workload uses the same inputs for all binaries, includes
synchronization, and verifies file contents after remounting XFS, dynfilefs and
FAT32. A separate case writes 1000 nonzero 4 KiB regions spaced 512 KiB apart,
then checks their contents and intervening zeros after remounting. Both cases
record allocated storage space and run `fsck.fat -n` while the storage exists.
The image resides on ext4; this measures the FAT32 stack in a VM, not a physical
USB drive. Backing-file caching remains enabled.

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

## libfuse 3 and format 501 trial, 2026-10-12

The comparison starts from the already optimized `a43e5f9` source, using
libfuse 2.9.9 and format 500. `4162631` switches to libfuse 3.18.3 and negotiates
up to 1 MiB transfers while retaining 4 KiB blocks. `6eb3678` changes allocation
blocks to 512 KiB, aligns each part's data area to 512 KiB and uses format 501.
Format 400 and 500 storage is rejected by the final version. Synchronization
logic is unchanged throughout this comparison; preallocation is absent.

All three benchmark binaries were rebuilt with their `static/build.sh`, using
the same pinned GCC 11.2.1 20211120 compiler, musl 1.2.6, optimization flags and
UPX 5.2.1. The trial's 512 KiB binary is the measured 32-bit static build.
The VM used Debian 13, kernel 7.2.9-slax, 16 vCPUs and 18 GiB configured RAM
(18425100 KiB usable). The FAT32 image was on ext4; copy inputs and the direct-I/O
microbenchmark's backing files were on tmpfs.

The first post-restart FUSE 2 microbenchmark contained severe stalls: its median
1 MiB read was only 42 MiB/s. Repeating the baseline after both FUSE 3 builds
produced three tightly grouped samples of 2517–2544 MiB/s. The table uses that
baseline rerun; the original samples and a preliminary pre-restart run are also
retained in the JSON. These are measurements over RAM with warm backing cache.

| tmpfs workload, median | FUSE 2 / 4 KiB, rerun | FUSE 3 / 4 KiB | FUSE 3 / 512 KiB |
| --- | ---: | ---: | ---: |
| 128 KiB direct read | 2194 MiB/s | 2220 MiB/s | 2267 MiB/s |
| 128 KiB overwrite | 1548 MiB/s | 1304 MiB/s | 1543 MiB/s |
| 1 MiB direct read | 2520 MiB/s | 4582 MiB/s | 4745 MiB/s |
| 1 MiB overwrite | 1613 MiB/s | 1980 MiB/s | 2028 MiB/s |
| Eight-worker 4 KiB random read | 39338 IOPS | 48664 IOPS | 50017 IOPS |
| Eight-worker 4 KiB random overwrite | 24312 IOPS | 22654 IOPS | 24822 IOPS |
| 4 KiB overwrite plus fsync | 13306 IOPS | 14433 IOPS | 15313 IOPS |

Larger FUSE transfers improve the 1 MiB workloads. Changing allocation blocks
from 4 KiB to 512 KiB adds only 3.5% to 1 MiB reads and 2.4% to 1 MiB overwrites
in this run. A debug mount confirmed actual 1048576-byte FUSE READ and WRITE
requests. Separate 128 KiB traces still use 128 backing writes and 256 splice
calls for 16 MiB per direction across all variants.

The FAT32/XFS matrix used three fresh filesystems per binary, identical source
contents and alternating binary order. Both copy phases include syncfs; reads
occur after remounting XFS, dynfilefs and FAT32. All nine repetitions passed
content SHA-256 and zero-gap checks, with 36 successful `fsck.fat -n` checks.

| FAT32/XFS workload, median | FUSE 2 / 4 KiB | FUSE 3 / 4 KiB | FUSE 3 / 512 KiB |
| --- | ---: | ---: | ---: |
| Copy 1 GiB and syncfs | 21.83 s | 24.23 s | 18.58 s |
| Copy 1000 x 16 KiB, eight workers, and syncfs | 4.369 s | 3.869 s | 2.139 s |
| Read 1 GiB after remount | 684 MiB/s | 407 MiB/s | 421 MiB/s |
| Allocated backing space after both copies | 1048.03 MiB | 1048.03 MiB | 1048.94 MiB |
| Allocated space for 1000 spaced 4 KiB writes | 5.41 MiB | 5.41 MiB | 500.50 MiB |

FAT32 timing results are exploratory: large-file copies ranged from 7.84–48.02 s,
19.62–58.28 s and 9.61–63.49 s respectively. VirtualBox continued reporting
timer catch-up warnings after the restart, despite zero guest memory-pressure
events and ample host RAM. These results do not establish a reliable disk-write
speedup. XFS reads were slower with FUSE 3 in this setup. The measured space
amplification is consistent across repetitions: spaced writes use about 93 times
more FAT32 space with 512 KiB blocks. For this reason, the larger-block format
is an experiment with a substantial sparse-write cost, not a general speed win.

Validation also passed callback fault/concurrency tests on 32-bit musl, native
64-bit AddressSanitizer/UndefinedBehaviorSanitizer tests, integration and ext4
loop tests, format 400/500 rejection without modifying their files, and format
501 interoperability between the 32-bit static and 64-bit autotools builds.
The intermediate FUSE 3 build retained format-500 compatibility in both directions.
The updated static build script and ordinary autogen/configure/make build passed.

[fuse3-results.json](fuse3-results.json) records binary hashes, source commits,
all samples, FAT checks, traces and the measurement limitations. Earlier FAT
runs affected by HostMemoryLow or cgroup throttling were excluded.

After this trial, the current implementation returned to 4 KiB blocks and the
original format-500 index layout, retaining libfuse 3 and transfers up to 1 MiB.
Version 5.01 accepts version-5.00 storage and rejects formats 400 and 501.
The 512 KiB measurements above remain a record of the discarded experiment.
The rebuilt static i686 binary passed callback fault/concurrency tests,
integration/remount tests and format-500 interoperability in both directions.
The spaced-write FAT32 fixture again used 5.41 MiB and passed persistence and fsck checks.
