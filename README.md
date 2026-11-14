# dynfilefs

FUSE filesystem exposing a virtual disk as `virtual.dat`. Backing files grow as data
is written and can be split to fit FAT32.

## Usage

```sh
dynfilefs -f storage_file -m mount_dir [-s size_MiB] [-p split_MiB] [-d] [--restore NAME]
```

- `-f`: backing metadata file; data parts use `.0`, `.1`, etc.
- `-m`: empty mount directory.
- `-s`: virtual size, required for new storage. Larger values grow existing storage;
  `+N` adds N MiB. Smaller sizes are ignored.
- `-p`: maximum data-part size, including indexes. Defaults to 4088 MiB for FAT32;
  existing storage keeps its original setting.
- `-d`: foreground debug mode. See `--help` for details.
- `-i`, `--info`: inspect saved geometry and reusable capacity without mounting or changing it.
- `--snapshots`: inspect the saved history tree before mounting.
- `--json`: JSON output for `--info` or `--snapshots`.
- `--restore NAME` or `-o restore=NAME`: start a new mounted session from a named snapshot.

```sh
dynfilefs -i -f /var/lib/changes.dat
```

Prints `key:value` lines: `version`, `size`, `sizeMiB`, `split`, `splitMiB`,
`blockSize`, `reusableBytes` and `currentParent`. Sizes without `MiB` are exact bytes;
MiB values are integers rounded down. `currentParent` is a stable 16-digit hexadecimal
snapshot ID, or `none`; JSON uses a string or `null`. Inspection includes the latest
complete checkpoint/log records without publishing recovery or creating a lock.
An actively mounted store returns `EBUSY`: use its live `stats` and `snapshots.json`
files instead. Errors go to stderr with a nonzero exit status.

Create a new 1 GiB disk:

```sh
mkdir -p /mnt/changes /mnt/data
dynfilefs -f /var/lib/changes.dat -m /mnt/changes -s 1024
mke2fs -F /mnt/changes/virtual.dat   # Format only when creating a new disk.
mount -o loop /mnt/changes/virtual.dat /mnt/data
```

To reuse it, run the mount commands without formatting. Unmount the inner filesystem first:

```sh
umount /mnt/data
umount /mnt/changes
```

Equivalent `/etc/fstab` entries:

```fstab
/var/lib/changes.dat /mnt/changes dynfilefs size=1024,split=4088 0 0
/mnt/changes/virtual.dat /mnt/data auto loop 0 0
```

With `-f /var/lib/changes.dat`, files in `/var/lib` can include:

```text
changes.dat           Metadata
changes.dat.0         Data and index pages, up to the configured split size
changes.dat.1         Further parts, created as needed
changes.dat.snapshots/ Named snapshot metadata, sharing the numbered data parts
    0000000000000001.snap
    0000000000000002.snap
    0000000000000003.snap
changes.dat.lock      Mount lock; removed after clean unmount
changes.dat.log       Metadata log, up to 32 MiB
changes.dat.work      Compacted checkpoint
changes.dat.next      Temporary metadata being written
changes.dat.migrate   Temporary conversion marker
```

Only needed files are created. Clean unmount leaves the main metadata file,
numbered data parts and snapshot directory. Numbered parts retain their lengths and
allocated space for reuse. Crash-recovery files remain until a successful next mount.

Keep the main metadata file, all numbered parts and the snapshot directory together.
Snapshots share backing blocks and are not independent backups.

Mounted stores retain directory handles instead of relying on their original absolute
paths. Writes, snapshot creation and clean unmount therefore keep working across
`pivot_root`, including when the store supplies a live system's writable layer.
Automatic filesystem freezing still requires accessible `/proc`, `/sys` and `/dev`
in the daemon's current root and an unchanged mount/device topology during creation.

## Sessions and history

There is one current state and a history of immutable snapshots. A normal mount
continues the current state. At startup, dynfilefs ensures that state has a saved
point; at clean shutdown it saves another point only if the state changed. The end
of one session is therefore reused as the beginning of the next. Empty sessions do
not create duplicate points. Unmount the inner filesystem before dynfilefs so a
clean-end point includes the filesystem's clean shutdown.

Automatic names have the reserved `auto-` prefix, a UTC timestamp and a unique ID.
The ten newest automatic live points are retained; open points and the current
parent are protected, so the limit may temporarily be exceeded. Manually named
points remain until explicitly deleted. Automatically removed points retain small
history records but no references to their former data blocks.

After interruption, the current state recovers the last successful `fsync`. No
clean-end point is created by the interrupted process. Startup can save the recovered
block state, which may still need the inner filesystem's ordinary journal replay.
Such a point is crash-consistent, not necessarily suitable for a no-recovery read-only
filesystem mount. To recover its filesystem, restore it as the current writable disk
and mount the inner filesystem normally; the saved point itself remains unchanged.

Before mounting, inspect the tree or get records for an application:

```sh
dynfilefs -f /var/lib/changes.dat --snapshots
dynfilefs -f /var/lib/changes.dat --snapshots --json
```

JSON contains `currentParent` and a `snapshots` array. Each node has `id`, `parent`,
`name`, `created` (Unix seconds), `deleted` and `automatic`. IDs are strings so they
remain exact in JavaScript. The tree marks the snapshot on which the current state
is based; it does not mean that unsaved current writes equal that snapshot.

Restoring B and creating D preserves C and records a new branch:

```text
A
└── B
    ├── C
    └── D
```

Deleting B keeps its stable ID, parent and display name as a history-only node. C
and D keep their original ancestry. B's saved content becomes unavailable, and
its exclusive blocks can be reused. The display name B can later be used for a
different point with a new ID. Historical nodes do not pin data blocks.

## Named snapshots

```sh
echo 'create before-update' > /mnt/changes/snapshot
cat /mnt/changes/snapshot
ls /mnt/changes/snapshots/
```

Reading `snapshot` returns one saved name per line. Each write must contain one complete
`create NAME` or `delete NAME` command, with an optional final newline. Names may contain
international text and spaces; shell-quote them as needed. Names cannot end in a dot or
space, contain control characters, path separators or FAT-incompatible punctuation,
or exceed 250 UTF-8 bytes. The `auto-` prefix is reserved for system-created points.
Creation refuses an existing live name. Each saved point has its own complete mapping
and allocation metadata at
`changes.dat.snapshots/ID.snap`; its UTF-8 name is stored inside that checksummed
record. Unchanged data blocks are shared. Deleted points retain only a 4096-byte
history record with no mapping or allocation bitmap.

Manual creation automatically freezes a directly loop-mounted ext4 or XFS filesystem using
`virtual.dat`, saves the durable snapshot and resumes the filesystem. Detection uses
the backing file's device/inode identity and mounts visible in dynfilefs's mount
namespace. It requires permission to freeze the filesystem, normally root. The helper
also resumes its owned freezes if dynfilefs exits unexpectedly. A loop with partitions,
device-mapper users, an unsupported filesystem or no accessible direct mount makes
creation fail. Keep mount and device topology unchanged during creation.
With no matching loop device, a raw virtual-disk snapshot is allowed;
the caller must coordinate any other writers or users not covered by direct-loop
detection. Concurrent snapshot commands return `EBUSY` while another command runs.

To return to a saved point, first unmount the inner filesystem and dynfilefs, then:

```sh
dynfilefs -f /var/lib/changes.dat -m /mnt/changes --restore before-update
mount -o loop /mnt/changes/virtual.dat /mnt/data
```

The saved point remains unchanged; new writes form the current state starting from it.
Other named points remain available; restoring never deletes newer points. Restore
selects the saved virtual size, including a smaller size than the previous current
state; `-s` can grow it as usual. Current
changes that must remain available should be saved as another named point first.

Saved images are also available at `snapshots/NAME.dat` while the current disk stays
writable. These files are immutable virtual views, not additional physical disk copies.
For a snapshot created while the inner filesystem was frozen:

```sh
mkdir -p /mnt/old-state
mount -t xfs -o loop,ro,nouuid,norecovery \
    /mnt/changes/snapshots/before-update.dat /mnt/old-state
# For ext4, use: mount -t ext4 -o loop,ro,noload ...
umount /mnt/old-state
echo 'delete before-update' > /mnt/changes/snapshot
```

An open snapshot image, including one held by a loop mount, cannot be deleted (`EBUSY`).
After closing it, deletion releases blocks needed only by that point for future writes
from any restored state. Blocks still used by the current state, another snapshot,
a durable crash checkpoint or outstanding descriptor-backed reads remain protected.
Deletion does not freeze the inner filesystem. There is no data compaction, hole
punching or physical space reclamation
of data parts; freed slots are reused within the existing numbered files.

## Reusable capacity

```sh
dynfilefs -f /var/lib/changes.dat --info --json
cat /mnt/changes/stats
cat /mnt/changes/snapshots.json
```

`reusableBytes` counts physical 4096-byte slots in the existing numbered files that
the allocator can immediately use. It excludes every block still needed by the
current state, live snapshots, the durable crash checkpoint or a pending
descriptor-backed read. Live `stats` reflects unsynced allocations too; offline
inspection requires an unmounted store and reports capacity available for its next
mount. Legacy format-500 inspection reports zero until conversion.

The allocator uses existing free slots before extending a data part or creating a
new one. Index pages and copy-on-write replacements consume the same reserve, so
`reusableBytes` is not an exact allowance for new files inside XFS/ext4. Main metadata,
the log and new snapshot records may allocate space independently. Sparse or
copy-on-write host filesystems can also allocate storage while overwriting an existing
part range. No data compaction, hole punching or physical shrinking is performed.

## Existing storage

Format 500 converts automatically on its first mount, without moving payload data.
Legacy split sizes must be an even number of MiB. The resulting format 600 records
stable snapshot identities and parent links and cannot be opened by older versions.

## Build

Requires libfuse 3.8+ development headers, pkg-config and autotools:

```sh
./autogen.sh
./configure
make
```

For a static Linux i686 binary at `static/dynfilefs`:

```sh
sh static/build.sh
```

The static build requires an x86 Linux host, make, curl, tar, gzip, xz, sha256sum,
meson, ninja and pkg-config. `JOBS=2` limits parallel builds; `PACK=0` skips UPX.

Run the snapshot integration checks on Linux with FUSE, loop-mount privileges and
e2fsprogs; XFS checks also require xfsprogs:

```sh
python3 tests/snapshots.py /absolute/path/to/dynfilefs
```

Use `--skip-loop` to run only the raw-image and catalog checks.

Test directory renames and real root transitions with a C compiler, unshare and
both filesystem toolsets (the test isolates its mounts and processes):

```sh
python3 tests/pivot-root.py /absolute/path/to/dynfilefs
```

[Tests and benchmarks](benchmarks/README.md) · [Stable releases](https://github.com/Tomas-M/dynfilefs/releases)

Development version: 6.02, storage format 600.
