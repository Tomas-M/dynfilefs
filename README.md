# dynfilefs
Fuse filesystem for dynamically-enlarged file (can be mounted as loop device too)

usage: ./dynfilefs -f storage_file -m mount_dir [ -s size_MB ] [ -p split_size_MB ] [ -d ]

Mount filesystem to [mount_dir], provide a virtual file [mount_dir]/virtual.dat of size [size_MB]
All changes made to virtual.dat file are stored to [storage_file] file(s)
```
  -d                       - Debug mode; do not fork to background

  --file [storage_file]
  [storage_file]
  -f [storage_file]        - Path to the file where changes to the virtual file will be stored.
                           - The storage file is created with the provided name to store metadata,
                             and then additional storage files are created with the same base name
                             with extension suffixes such as .0, .1, .2, etc.
                           - If the storage exists, it will be reused.

  --mountdir [mount_dir]
  [mount_dir]
  -m [mount_dir]           - Specifies the directory where the filesystem will be mounted.
                           - The directory must be empty, or the mount operation will be refused.

  --size [size_MB]
  -o size=[size_MB]
  -s [size_MB]             - Sets the size of the virtual.dat file in MB.
                           - If storage file exists, you can specify bigger size_MB than before,
                             in that case the size of virtual file will be enlarged.
                           - If the specified size_MB is smaller than before, it will be ignored
                             and the previous stored value of size_MB will be reused.
                           - If the size is specified as +size_MB (note the plus sign prefix),
                             then the virtual file will grow by size_MB if storage_file exists.

  --split [split_size_MB]
  -o split=[split_size_MB]
  -p [split_size_MB ]      - Sets the maximum data size per storage file. Multiple files
                             will be created if [size_MB] > [split_size_MB].
                             Beware that actual file size (including internal indexes) may be
                             bigger than split_size_MB, so use max 4088 on FAT32 to be safe,
                             because FAT32 does not support individual files bigger than 4GB.
                           - This parameter is ignored if storage file exists,
                             in that case the previous stored value is reused.
```

Example usage:

    ./dynfilefs -f /tmp/changes.dat -s 1024 -m /mnt
    mke2fs -F /mnt/virtual.dat
    mount -o loop /mnt/virtual.dat /mnt

Usage in fstab
```
  /var/lib/changes.dat /var/lib/changes dynfilefs size=1024,split=1000 0 0
  /var/lib/changes/virtual.dat /extra auto loop 0 0 
```
How to compile:

    ./autogen.sh
    ./configure
    make


How to compile statically:

    sh static/build.sh

The script builds a fully static 32-bit x86 (i686) binary at `static/dynfilefs`
on an x86 or x86-64 Linux host that can run 32-bit executables. It downloads a
standalone compiler, builds musl and libfuse with verified SHA-256 checksums,
and packs the result with UPX. No Buildroot, root access or host multilib is needed.
Required tools: make, curl, tar, gzip, xz and standard shell utilities.

Use `JOBS=2 sh static/build.sh` to limit parallel builds, or `PACK=0 sh static/build.sh`
to skip compression. Archives placed in `static/sources/` are reused after checksum
verification. The previous binary is replaced only after all checks pass.



# WARNING!


Master branch is work in progress, may be broken. See releases for "stable" versions.
Latest version is 4.03 (2023)
Releases can be found here: https://github.com/Tomas-M/dynfilefs/releases
