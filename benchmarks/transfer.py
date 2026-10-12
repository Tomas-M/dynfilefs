#!/usr/bin/env python3
import argparse
import contextlib
import json
import os
from pathlib import Path
import shutil
import statistics
import subprocess
import tempfile
import time

from benchmarks.copy import measure, prepare
from benchmarks.run import mounted, process_args


# Format only an owned regular image, then check FAT while the storage files still exist.
@contextlib.contextmanager
def fat_volume(parent):
    root=Path(tempfile.mkdtemp(prefix='dynfilefs-transfer-fat-', dir=parent))
    image=root/'image.fat'
    with image.open('xb') as file:
        file.truncate(8192*1024*1024)
    subprocess.run(['mkfs.fat', '-F', '32', '-s', '64', str(image)], check=True, stdout=subprocess.DEVNULL)
    mount=root/'fat'
    mount.mkdir()
    command=['mount', '-t', 'vfat', '-o', 'loop,noatime', str(image), str(mount)]
    checks=[]

    # FUSE and loop references can outlive unmount and even the daemon's cmdline.
    def unmount():
        for _ in range(750):
            busy=any(process_args(path).startswith(b'@') and str(mount).encode() in process_args(path) for path in Path('/proc').iterdir() if path.name.isdigit())
            if not busy:
                result=subprocess.run(['umount', str(mount)], capture_output=True)
                if not result.returncode:
                    return
            time.sleep(0.02)
        raise RuntimeError('FAT32 test fixture is still busy after FUSE unmount: '+str(mount))

    # Reopen the backing filesystem itself, including its persisted cluster chains.
    def remount():
        unmount()
        result=subprocess.run(['fsck.fat', '-n', str(image)], capture_output=True, text=True)
        checks.append((result.stdout+result.stderr).replace(str(image), 'image.fat'))
        if result.returncode:
            raise RuntimeError(checks[-1])
        subprocess.run(command, check=True)

    subprocess.run(command, check=True)
    try:
        yield mount, remount, checks
    finally:
        if os.path.ismount(mount):
            unmount()
        result=subprocess.run(['fsck.fat', '-n', str(image)], capture_output=True, text=True)
        checks.append((result.stdout+result.stderr).replace(str(image), 'image.fat'))
        if result.returncode:
            raise RuntimeError(checks[-1])
        # Never traverse a mounted fixture or discard an image after failed fsck.
        shutil.rmtree(root)


# Expose allocation amplification with one 4 KiB write in each distinct 512 KiB region.
def measure_gaps(binary, parent, remount):
    payload=bytes(range(256))*16
    with tempfile.TemporaryDirectory(prefix='dynfilefs-transfer-gaps-', dir=parent) as folder:
        root=Path(folder)
        with mounted(binary, root) as (path, pid):
            with path.open('r+b', buffering=0) as file:
                started=time.monotonic()
                for block in range(1000):
                    assert os.pwrite(file.fileno(), payload, block*512*1024)==len(payload)
                os.fsync(file.fileno())
                seconds=time.monotonic()-started
            parts=[path.stat() for path in root.glob('storage.*')]
            allocated_bytes=sum(part.st_blocks*512 for part in parts)
            storage_bytes=sum(part.st_size for part in parts)
        remount()
        with mounted(binary, root) as (path, pid):
            with path.open('rb', buffering=0) as file:
                for block in range(1000):
                    assert os.pread(file.fileno(), 4096, block*512*1024)==payload
                    assert os.pread(file.fileno(), 4096, block*512*1024+8192)==bytes(4096)
    result={'seconds':seconds, 'allocated_bytes':allocated_bytes, 'storage_bytes':storage_bytes}
    print('Spaced writes: '+json.dumps(result)+'; data and zeros after FAT remount PASS', flush=True)
    return result


if __name__=='__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('output')
    parser.add_argument('--binary', action='append', required=True, help='LABEL=/absolute/path/to/binary')
    parser.add_argument('--directory', required=True)
    parser.add_argument('--source-directory', required=True)
    parser.add_argument('--repeats', type=int, default=3)
    args=parser.parse_args()
    binaries={label:str(Path(path).resolve()) for label, path in (value.split('=', 1) for value in args.binary)}
    parent=Path(args.directory)
    source=Path(args.source_directory)
    parent.mkdir(parents=True, exist_ok=True)
    source.mkdir(parents=True, exist_ok=True)
    variants={label:{'xfs':[], 'gaps':[]} for label in binaries}
    data={'filesystem':'XFS / dynfilefs / FAT32 / loop image', 'image_mib':8192, 'cluster_bytes':32768, 'xfs_virtual_mib':3072, 'xfs_split_mib':256, 'large_bytes':1024**3, 'small_files':1000, 'small_bytes':16384, 'workers':8, 'gap_writes':1000, 'gap_write_bytes':4096, 'gap_stride_bytes':524288, 'repeats':args.repeats, 'variants':variants}
    data['image_backing_filesystem']=subprocess.check_output(['findmnt', '-n', '-o', 'FSTYPE', '-T', str(parent)], text=True).strip()
    with tempfile.TemporaryDirectory(prefix='dynfilefs-transfer-source-', dir=source) as folder:
        large, small=prepare(Path(folder))
        for repeat in range(args.repeats):
            labels=list(binaries) if repeat%2==0 else list(reversed(binaries))
            for label in labels:
                print(f'{label}, repetition {repeat+1}', flush=True)
                with fat_volume(parent) as (fat, remount, checks):
                    sample=measure(binaries[label], fat, large, small, 8, remount)
                sample['fsck']=checks
                variants[label]['xfs'].append(sample)
                with fat_volume(parent) as (fat, remount, checks):
                    sample=measure_gaps(binaries[label], fat, remount)
                sample['fsck']=checks
                variants[label]['gaps'].append(sample)
                Path(args.output).write_text(json.dumps(data, indent=2)+'\n')
    for variant in variants.values():
        for case in ('xfs', 'gaps'):
            variant[case+'_median']={key:statistics.median(sample[key] for sample in variant[case]) for key in variant[case][0] if key!='fsck'}
    Path(args.output).write_text(json.dumps(data, indent=2)+'\n')
