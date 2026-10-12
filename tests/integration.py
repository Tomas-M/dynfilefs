#!/usr/bin/env python3
import argparse
import concurrent.futures
import contextlib
import os
from pathlib import Path
import random
import struct
import subprocess
import tempfile
import time


# Mount only disposable storage and always unmount it before cleanup.
@contextlib.contextmanager
def mounted(binary, storage, mount, size=8, split=3):
    mount.mkdir(exist_ok=True)
    subprocess.run([binary, '-f', str(storage), '-m', str(mount), '-s', str(size), '-p', str(split)], check=True)
    for _ in range(100):
        if (mount/'virtual.dat').exists():
            break
        time.sleep(0.02)
    assert (mount/'virtual.dat').exists(), 'Mount did not become ready'
    try:
        yield mount/'virtual.dat'
    finally:
        subprocess.run(['umount', str(mount)], check=True)


# Compare persisted data with an independent byte-array oracle after remounting.
def check_storage(binary, root):
    storage, mount=root/'storage', root/'mount'
    expected=bytearray(8*1024*1024)
    rng=random.Random(5231)
    with mounted(binary, storage, mount) as path:
        fd=os.open(path, os.O_RDWR)
        initial=(root/'storage.0').stat().st_size
        assert os.pwrite(fd, bytes(131072), 0)==131072
        os.fsync(fd)
        assert (root/'storage.0').stat().st_size==initial, 'Zero write allocated storage'
        operations=[(0, 1), (4095, 8193), (3*1024*1024-111, 131072), (len(expected)-512, 512)]
        operations += [(rng.randrange(len(expected)-131072), rng.choice([31, 512, 4096, 65536, 131072])) for _ in range(80)]
        for offset, size in operations:
            data=rng.randbytes(size)
            assert os.pwrite(fd, data, offset)==size
            expected[offset:offset+size]=data
        assert os.pwrite(fd, bytes(8192), 4096)==8192
        expected[4096:12288]=bytes(8192)
        os.fsync(fd)
        assert os.pread(fd, len(expected), 0)==expected
        assert os.pread(fd, 1024, len(expected))==b''
        os.close(fd)
    with mounted(binary, storage, mount) as path:
        fd=os.open(path, os.O_RDWR)
        assert os.pread(fd, len(expected), 0)==expected
        # Force new allocation after a remount ending in a partial physical block.
        offset=7*1024*1024+173
        assert os.pwrite(fd, b'partial', offset)==7
        expected[offset:offset+7]=b'partial'
        os.fsync(fd)
        os.close(fd)
    with mounted(binary, storage, mount) as path:
        fd=os.open(path, os.O_RDWR)
        assert os.pread(fd, len(expected), 0)==expected
        os.pwrite(fd, b'after-remount', 6*1024*1024)
        expected[6*1024*1024:6*1024*1024+13]=b'after-remount'
        os.fsync(fd)
        os.close(fd)
    with mounted(binary, storage, mount) as path:
        assert path.read_bytes()==expected
        # Direct reads bypass the FUSE page cache and exercise descriptor-backed buffers.
        subprocess.run(['dd', 'if='+str(path), 'of='+str(root/'direct-read'), 'bs=128K', 'iflag=direct', 'status=none'], check=True)
        assert (root/'direct-read').read_bytes()==expected
    print('PASS: sparse zeros, partial writes, split boundaries, EOF, direct reads, remounts', flush=True)


# An indexed partial tail must read as zeros beyond the backing file's EOF.
def check_partial_tail(binary, root):
    expected=bytearray(1024*1024)
    for offset, data in ((0, b'A'*17), (4096, b'B'*31)):
        with mounted(binary, root/'tail', root/'tail-mount', 1, 1) as path:
            fd=os.open(path, os.O_RDWR)
            assert os.pwrite(fd, data, offset)==len(data)
            expected[offset:offset+len(data)]=data
            os.fsync(fd)
            os.close(fd)
            subprocess.run(['dd', 'if='+str(path), 'of='+str(root/'tail-read'), 'bs=128K', 'iflag=direct', 'status=none'], check=True)
            assert (root/'tail-read').read_bytes()==expected
        with mounted(binary, root/'tail', root/'tail-mount', 1, 1) as path:
            assert path.read_bytes()==expected
    print('PASS: partial physical tail and allocation after remount', flush=True)


# Race allocations and overwrites against fsync without overlapping writer ranges.
def check_concurrency(binary, root):
    expected=bytearray(8*1024*1024)
    with mounted(binary, root/'parallel', root/'parallel-mount') as path:
        fd=os.open(path, os.O_RDWR)

        # Each worker owns its range so the final bytes have one unambiguous value.
        def writer(worker):
            rng=random.Random(worker)
            for _ in range(128):
                offset=(worker*128+rng.randrange(128))*4096
                data=rng.randbytes(4096)
                assert os.pwrite(fd, data, offset)==4096
                expected[offset:offset+4096]=data

        # Repeated barriers must not clear changes made by concurrent writers.
        def synchronizer():
            for _ in range(64):
                os.fsync(fd)

        with concurrent.futures.ThreadPoolExecutor(max_workers=9) as pool:
            futures=[pool.submit(writer, worker) for worker in range(8)]
            futures.append(pool.submit(synchronizer))
            for future in futures:
                future.result()
        os.fsync(fd)
        os.close(fd)
    with mounted(binary, root/'parallel', root/'parallel-mount') as path:
        assert path.read_bytes()==expected
    print('PASS: parallel allocations/overwrites and concurrent fsync', flush=True)


# Verify the intended loop-device use with an actual ext4 filesystem and remount.
def check_loop(binary, root):
    inner=root/'inner'
    inner.mkdir()
    expected={name:random.Random(i).randbytes(1024*1024) for i, name in enumerate(('data', 'žluťoučký', '文件'))}
    for first in (True, False):
        with mounted(binary, root/'loop', root/'loop-mount', 32, 3) as path:
            if first:
                subprocess.run(['mkfs.ext4', '-q', '-F', str(path)], check=True)
            subprocess.run(['mount', '-o', 'loop', str(path), str(inner)], check=True)
            try:
                for name, data in expected.items():
                    target=inner/name
                    if first:
                        with target.open('wb') as file:
                            file.write(data)
                            file.flush()
                            os.fsync(file.fileno())
                    assert target.read_bytes()==data
            finally:
                subprocess.run(['umount', str(inner)], check=True)
    print('PASS: loop-mounted ext4, file fsync and remount', flush=True)


# Reject the old format before changing either storage file's contents.
def check_old_format(binary, root):
    for main_version in (400, 500):
        storage=root/('old-'+str(main_version))
        header=bytearray(4096)
        struct.pack_into('<qqq', header, 2048, main_version, 3*1024*1024, 8*1024*1024)
        storage.write_bytes(header)
        split=Path(str(storage)+'.0')
        old=bytearray(header)
        struct.pack_into('<q', old, 2048, 400)
        split.write_bytes(old+bytes(6144))
        before=storage.read_bytes(), split.read_bytes()
        result=subprocess.run([binary, '-f', str(storage), '-m', str(root/'mount')], capture_output=True)
        assert result.returncode!=0 and b'incompatible data format version 400' in result.stdout
        assert before==(storage.read_bytes(), split.read_bytes())
    print('PASS: format 400 rejected without rewriting storage', flush=True)


# Check both directions of format-500 compatibility across the optimized callbacks.
def check_compatibility(binary, baseline, root):
    expected=bytearray(8*1024*1024)
    for phase, program in enumerate((baseline, binary, baseline)):
        with mounted(program, root/'compatible', root/'compatible-mount') as path:
            assert path.read_bytes()==expected
            offset=phase*3*1024*1024+123
            data=random.Random(phase).randbytes(4127)
            fd=os.open(path, os.O_RDWR)
            assert os.pwrite(fd, data, offset)==len(data)
            os.fsync(fd)
            os.close(fd)
            expected[offset:offset+len(data)]=data
    with mounted(binary, root/'compatible', root/'compatible-mount') as path:
        assert path.read_bytes()==expected
    print('PASS: format-500 compatibility in both directions', flush=True)


if __name__=='__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('binary')
    parser.add_argument('--baseline')
    args=parser.parse_args()
    binary=str(Path(args.binary).resolve())
    with tempfile.TemporaryDirectory(prefix='dynfilefs-test-') as folder:
        root=Path(folder)
        check_storage(binary, root)
        check_partial_tail(binary, root)
        check_concurrency(binary, root)
        check_loop(binary, root)
        check_old_format(binary, root)
        if args.baseline:
            check_compatibility(binary, str(Path(args.baseline).resolve()), root)
