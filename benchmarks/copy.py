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
def mounted(binary, root, create):
    mount, inner=root/'mount', root/'xfs'
    mount.mkdir(exist_ok=True)
    inner.mkdir(exist_ok=True)
    subprocess.run([binary, '-f', str(root/'storage'), '-m', str(mount), '-s', '3072', '-p', '256'], check=True)
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
def measure(binary, parent, large, small, workers):
    expected_large=digest(large)
    expected_small={path.name:digest(path) for path in small}
    with tempfile.TemporaryDirectory(prefix='dynfilefs-copy-', dir=parent) as folder:
        root=Path(folder)
        with mounted(binary, root, True) as inner:
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
        with mounted(binary, root, False) as inner:
            assert digest(inner/'large')==expected_large
            assert {path.name:digest(path) for path in (inner/'small').iterdir()}==expected_small
    result={'large_seconds':large_seconds, 'large_mib_s':1024/large_seconds, 'small_seconds':small_seconds, 'small_files_s':1000/small_seconds}
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
    args=parser.parse_args()
    binary=str(Path(args.binary).resolve())
    parent=Path(args.directory)
    source=Path(args.source_directory)
    parent.mkdir(parents=True, exist_ok=True)
    source.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='dynfilefs-copy-source-', dir=source) as folder:
        large, small=prepare(Path(folder))
        samples=[measure(binary, parent, large, small, args.workers) for _ in range(args.repeats)]
    result={'filesystem':'XFS on loop over dynfilefs', 'backing_filesystem':subprocess.check_output(['findmnt', '-n', '-o', 'FSTYPE', '-T', str(parent)], text=True).strip(), 'large_bytes':1024**3, 'small_files':1000, 'small_bytes':16384, 'workers':args.workers, 'samples':samples, 'median':{key:statistics.median(sample[key] for sample in samples) for key in samples[0]}}
    Path(args.output).write_text(json.dumps(result, indent=2)+'\n')
