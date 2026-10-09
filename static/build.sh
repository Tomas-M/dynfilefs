#!/bin/sh
set -eu
export LC_ALL=C

if [ "$(uname -s)" != Linux ]; then
    echo "Run this script on Linux (or inside WSL)." >&2
    exit 1
fi
for tool in make curl tar gzip xz sha256sum; do
    command -v "$tool" >/dev/null || { echo "Missing build tool: $tool" >&2; exit 1; }
done

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(dirname -- "$script_dir")
jobs=${JOBS:-$(getconf _NPROCESSORS_ONLN)}
pack=${PACK:-1}
case "$jobs" in ''|0|*[!0-9]*) echo "JOBS must be a positive integer." >&2; exit 1;; esac
case "$pack" in 0|1) ;; *) echo "PACK must be 0 or 1." >&2; exit 1;; esac

musl_version=1.2.6
fuse_version=2.9.9
upx_version=5.2.1
# UPX runs on the host; the compiler and target executable are always 32-bit x86.
case "$(uname -m)" in
    x86_64) upx_arch=amd64; upx_hash=402162aad30af47e60dbd767fb2e64ca394ace9727ba1f40283641f1d1b91657;;
    i?86) upx_arch=i386; upx_hash=44b505d881337ef17ad03f03fd80f2ecdebc3a36077e1dd260ae1b359c713ba5;;
    *) echo "This 32-bit x86 build requires an x86 or x86-64 Linux host." >&2; exit 1;;
esac

build_dir=$(mktemp -d "$script_dir/.build.XXXXXX")
# Replace the existing binary only after all checks pass; clean on failure too.
trap 'status=$?; if [ "$status" -ne 0 ] && [ -f "$build_dir/build.log" ]; then tail -n 60 "$build_dir/build.log" >&2; fi; rm -rf -- "$build_dir"' 0
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
cd "$build_dir"

# Verify local source archives or download the pinned upstream files.
download() {
    archive=${1##*/}
    if [ -f "$script_dir/sources/$archive" ]; then
        archive="$script_dir/sources/$archive"
    else
        curl --fail --location --retry 3 --connect-timeout 30 --output "$archive" "$1"
    fi
    printf '%s  %s\n' "$2" "$archive" | sha256sum -c -
    tar -xf "$archive"
}

# A no-argument invocation prints usage and exits before opening any storage.
check_binary() {
    ./dynfilefs >usage.txt 2>&1 || test "$?" -eq 1
    grep '^usage: ' usage.txt
}

download "https://musl.cc/i686-linux-musl-native.tgz" \
    978471bf7b8111dfd8c5559a23ef18b80bcd85936872f00424f1b7a5300580ee
export PATH="$build_dir/i686-linux-musl-native/bin:$PATH"
download "https://musl.libc.org/releases/musl-$musl_version.tar.gz" \
    d585fd3b613c66151fc3249e8ed44f77020cb5e6c1e635a616d3f9f82460512a
download "https://github.com/libfuse/libfuse/releases/download/fuse-$fuse_version/fuse-$fuse_version.tar.gz" \
    d0e69d5d608cc22ff4843791ad097f554dd32540ddc9bed7638cc6fea7c1b4b5
if [ "$pack" = 1 ]; then
    download "https://github.com/upx/upx/releases/download/v$upx_version/upx-$upx_version-${upx_arch}_linux.tar.xz" "$upx_hash"
fi

prefix="$build_dir/local"
export CFLAGS='-m32 -march=i686 -fno-pie -Os -g0 -ffunction-sections -fdata-sections -fno-unwind-tables -fno-asynchronous-unwind-tables'
export LDFLAGS='-m32 -static -no-pie -Wl,--gc-sections -Wl,--build-id=none'
echo "Building 32-bit x86 musl $musl_version..."
(
    cd "musl-$musl_version"
    ./configure --prefix="$prefix" --target=i686-linux-musl \
        --disable-shared --disable-optimize --enable-wrapper=gcc CC=gcc AR=ar RANLIB=ranlib
    make -j "$jobs"
    make install
) >build.log 2>&1

export CC="$prefix/bin/musl-gcc"
echo "Building static libfuse $fuse_version..."
(
    cd "fuse-$fuse_version"
    # Keep the runtime fusermount path independent of our temporary prefix.
    ./configure --prefix="$prefix" --bindir=/usr/bin --host=i686-linux-musl \
        --disable-shared --enable-static --disable-util --disable-example --disable-mtab
    make -j "$jobs"
    make install
) >>build.log 2>&1

echo "Linking 32-bit x86 DynFileFS..."
"$CC" $CFLAGS -std=gnu99 -D_FILE_OFFSET_BITS=64 -D_REENTRANT \
    -I"$prefix/include/fuse" "$project_dir/dynfilefs.c" "$prefix/lib/libfuse.a" \
    $LDFLAGS -pthread -lrt -ldl -o dynfilefs >>build.log 2>&1
strip --strip-all dynfilefs

# Require a static ELF32 executable before compression changes its ELF layout.
readelf -h dynfilefs >header.txt
if ! grep -Eq 'Class:[[:space:]]+ELF32' header.txt || ! grep -Eq 'Machine:[[:space:]]+Intel 80386' header.txt; then
    echo "The resulting binary is not 32-bit x86." >&2
    exit 1
fi
readelf -l dynfilefs >elf.txt
readelf -d dynfilefs >>elf.txt
if grep -Eq 'INTERP|\(NEEDED\)' elf.txt; then
    echo "The resulting binary is not fully static." >&2
    exit 1
fi
check_binary
echo "Unpacked size: $(wc -c < dynfilefs) bytes"
if [ "$pack" = 1 ]; then
    "./upx-$upx_version-${upx_arch}_linux/upx" --best --lzma dynfilefs
    "./upx-$upx_version-${upx_arch}_linux/upx" -t dynfilefs
    check_binary
fi
chmod 755 dynfilefs
mv -f dynfilefs "$script_dir/dynfilefs"
echo "Built $script_dir/dynfilefs ($(wc -c < "$script_dir/dynfilefs") bytes)"
