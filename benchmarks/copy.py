#!/usr/bin/env python3
import argparse
import concurrent.futures
import contextlib
import ctypes
import hashlib
import json
import os
from pathlib import Path
import shutil
import statistics
import subprocess
import tempfile
import time


# Flush only the dedicated test filesystem, including copied files and directory metadata.
def synchronize(path):
    fd=os.open(path, os.O_RDONLY)
    try:
        if ctypes.CDLL(None, use_errno=True).syncfs(fd):
            raise OSError(ctypes.get_errno(), 'syncfs failed')
    finally:
        os.close(fd)


# Hash outside the timed copy interval and again after both filesystems are remounted.
def digest(path):
    with path.open('rb') as file:
        return hashlib.file_digest(file, 'sha256').hexdigest()


# Create or reopen XFS inside disposable dynfilefs storage.
@contextlib.contextmanager
def mounted(binary, root, create, split=256, size=3072):
    mount, inner=root/'mount', root/'xfs'
    mount.mkdir(exist_ok=True)
    inner.mkdir(exist_ok=True)
    subprocess.run([binary, '-f', str(root/'storage'), '-m', str(mount), '-s', str(size), '-p', str(split)], check=True)
    try:
        if create:
            subprocess.run(['mkfs.xfs', '-q', '-f', str(mount/'virtual.dat')], check=True)
        subprocess.run(['mount', '-o', 'loop', str(mount/'virtual.dat'), str(inner)], check=True)
        try:
            yield inner
        finally:
            subprocess.run(['umount', str(inner)], check=True)
    finally:
        subprocess.run(['umount', str(mount)], check=True)


# Use real nonzero data; source files live on tmpfs to avoid source-disk contention.
def prepare(root):
    large=root/'large'
    with large.open('wb') as file:
        for _ in range(64):
            file.write(os.urandom(16*1024*1024))
    small=root/'small'
    small.mkdir()
    for index in range(1000):
        (small/f'file-{index:04d}').write_bytes(os.urandom(16384))
    return large, list(small.iterdir())


# Include XFS durability in copy timings, then verify persistence after remounting.
def measure(binary, parent, large, small, workers, split=256, size=3072, remount=None):
    expected_large=digest(large)
    expected_small={path.name:digest(path) for path in small}
    with tempfile.TemporaryDirectory(prefix='dynfilefs-copy-', dir=parent) as folder:
        root=Path(folder)
        with mounted(binary, root, True, split, size) as inner:
            synchronize(inner)
            started=time.monotonic()
            subprocess.run(['cp', '--reflink=never', str(large), str(inner/'large')], check=True)
            synchronize(inner)
            large_seconds=time.monotonic()-started
            target=inner/'small'
            target.mkdir()
            synchronize(inner)
            started=time.monotonic()
            with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
                list(pool.map(lambda path: shutil.copyfile(path, target/path.name), small))
            synchronize(inner)
            small_seconds=time.monotonic()-started
            parts=[path.stat() for path in root.glob('storage.*')]
            allocated_bytes=sum(part.st_blocks*512 for part in parts)
            storage_bytes=sum(part.st_size for part in parts)
        if remount:
            remount()
        with mounted(binary, root, False, split, size) as inner:
            started=time.monotonic()
            subprocess.run(['dd', 'if='+str(inner/'large'), 'of=/dev/null', 'bs=1M', 'iflag=direct', 'status=none'], check=True)
            read_seconds=time.monotonic()-started
            assert digest(inner/'large')==expected_large
            assert {path.name:digest(path) for path in (inner/'small').iterdir()}==expected_small
    result={'large_seconds':large_seconds, 'large_mib_s':1024/large_seconds, 'small_seconds':small_seconds, 'small_files_s':1000/small_seconds, 'read_seconds':read_seconds, 'read_mib_s':1024/read_seconds, 'allocated_bytes':allocated_bytes, 'storage_bytes':storage_bytes}
    print('XFS copy: '+json.dumps(result)+'; SHA-256 after remount PASS', flush=True)
    return result


if __name__=='__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('binary')
    parser.add_argument('output')
    parser.add_argument('--directory', required=True)
    parser.add_argument('--source-directory', required=True)
    parser.add_argument('--repeats', type=int, default=3)
    parser.add_argument('--workers', type=int, default=8)
    parser.add_argument('--compare', action='append', default=[], help='Additional binary to compare using the same inputs and alternating run order')
    args=parser.parse_args()
    binary=str(Path(args.binary).resolve())
    binaries=[binary]+[str(Path(path).resolve()) for path in args.compare]
    parent=Path(args.directory)
    source=Path(args.source_directory)
    parent.mkdir(parents=True, exist_ok=True)
    source.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='dynfilefs-copy-source-', dir=source) as folder:
        large, small=prepare(Path(folder))
        variants={path:[] for path in binaries}
        for repeat in range(args.repeats):
            for path in binaries if repeat%2==0 else reversed(binaries):
                print('Binary: '+Path(path).name, flush=True)
                variants[path].append(measure(path, parent, large, small, args.workers))
    result={'filesystem':'XFS on loop over dynfilefs', 'backing_filesystem':subprocess.check_output(['findmnt', '-n', '-o', 'FSTYPE', '-T', str(parent)], text=True).strip(), 'large_bytes':1024**3, 'small_files':1000, 'small_bytes':16384, 'workers':args.workers}
    result['variants']={Path(path).name:{'samples':samples, 'median':{key:statistics.median(sample[key] for sample in samples) for key in samples[0]}} for path, samples in variants.items()}
    Path(args.output).write_text(json.dumps(result, indent=2)+'\n')
