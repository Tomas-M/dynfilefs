# dynfilefs
Fuse filesystem for dynamically-enlarged file (can be mounted as loop device too)

usage: ./dynfilefs -f storage_file -m mount_dir [ -s size_MB ] [ -p split_size_MB ] [ -d ]

Mount filesystem to [mount_dir], provide a virtual file [mount_dir]/virtual.dat of size [size_MB]
All changes made to virtual.dat file are stored to [storage_file] file(s)

    -d                       - Do not fork to background, debug mode

    --mountdir [mount_dir]
    -m [mount_dir]           - Path to a directory where the fileszstem will be mounted
                             - The directory must be empty, else it will refuse to mount

    --file [storage_file]
    -f [storage_file]        - Path to a file where all changes will be stored
                             - If file exists, it will be used
                             - If file does not exist, it will be created empty

    --size [size_MB]
    -s [size_MB]             - The virtual.dat file will be size_MB big

    --split [split_size_MB]
    -p [split_size_MB ]      - Maximum data size per storage_file. Multiple storage files
                               will be created if [size_MB] > [split_size_MB].
                               Beware that actual file size (including index of offsets) may be
                               bigger than split_size_MB, so use max 4088 on FAT32 to be safe.

Example usage:

    ./dynfilefs -f /tmp/changes.dat -s 1024 -m /mnt
    mke2fs -F /mnt/virtual.dat
    mount -o loop /mnt/virtual.dat /mnt


How to compile:

    ./configure
    make


How to compile statically:

    DynFileFS can be statically compiled with buildroot, but it's a bit tricky.
    Read buildroot/README for more information. Tested on buildroot-2022.02.8
    Pre-built static binary for newest version can be found in ./static/ directory.



# WARNING!


Master branch is work in progress, may be broken. See releases for "stable" versions.
Latest version is 4.03 (2023)
Releases can be found here: https://github.com/Tomas-M/dynfilefs/releases
