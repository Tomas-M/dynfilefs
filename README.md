# dynfilefs

FUSE filesystem exposing a virtual disk as `virtual.dat`. Backing files grow as data
is written and can be split to fit FAT32.

## Usage

```sh
dynfilefs -f storage_file -m mount_dir [-s size_MiB] [-p split_MiB] [-d] [--norollback]
```

- `-f`: backing metadata file; data parts use `.0`, `.1`, etc.
- `-m`: empty mount directory.
- `-s`: virtual size, required for new storage. Larger values grow existing storage;
  `+N` adds N MiB. Smaller sizes are ignored.
- `-p`: maximum data-part size, including indexes. Defaults to 4088 MiB for FAT32;
  existing storage keeps its original setting.
- `-d`: foreground debug mode. See `--help` for details.
- `-i`, `--info`: inspect existing storage without mounting or changing it.
- `--norollback` or `-o norollback`: continue a session with pending rollback for this mount.

```sh
dynfilefs -i -f /var/lib/changes.dat
```

Prints `key:value` lines: `version`, `size`, `sizeMiB`, `split`, `splitMiB`,
`blockSize`, `rollback` (0 or 1). Sizes without `MiB` are exact bytes; MiB values
are integers rounded down. Works with formats 500 and 600, also while mounted. Reports the
latest saved metadata, including the log; unsynced growth is not yet visible.
Errors go to stderr with a nonzero exit status. Inspection does not perform rollback.

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
changes.dat.lock      Mount lock; removed after clean unmount
changes.dat.log       Metadata log, up to 32 MiB
changes.dat.work      Compacted checkpoint
changes.dat.rollback  Saved rollback request
changes.dat.next      Temporary metadata being written
changes.dat.migrate   Temporary conversion marker
```

Only needed files are created. Without a pending rollback, clean unmount leaves only
the metadata file and numbered data parts, reclaiming unused tails and empty parts.
Pending rollback or crash-recovery files remain until the next mount; keep them together
with the backing file.

## Rollback

```sh
echo 1 > /mnt/changes/rollback  # Discard this entire session at the next mount.
echo 0 > /mnt/changes/rollback  # Cancel the request.
cat /mnt/changes/rollback      # Read the current flag.
```

The flag is saved immediately. Rollback includes all writes during the current mount;
the mounted view stays usable. With rollback enabled, clean unmount saves the current
state and leaves the request and recovery files for the next mount. A normal next mount
restores the original state, reclaims unused space and resets the flag to `0`.
Without rollback, clean unmount keeps all changes; a crash recovers the last successful `fsync`.

Use `--norollback` to continue the saved state instead. The flag stays `1`, and the
original rollback point survives any number of continued mounts. A later mount without
the option discards changes from all those sessions. Writing `0` cancels the request;
the next clean unmount commits their combined state. With no pending request, the option
does nothing. After a crash, continuation recovers the last successful `fsync`.

## Existing storage

Format 500 converts automatically on its first mount, without moving payload data.
Legacy split sizes must be an even number of MiB. The resulting format 600 cannot be
opened by older versions.

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

[Tests and benchmarks](benchmarks/README.md) · [Stable releases](https://github.com/Tomas-M/dynfilefs/releases)

Development version: 6.00, storage format 600.
