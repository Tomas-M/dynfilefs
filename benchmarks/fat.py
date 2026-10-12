#!/usr/bin/env python3
import argparse
import contextlib
import json
import os
from pathlib import Path
import platform
import statistics
import struct
import subprocess
import tempfile

from benchmarks.copy import measure, prepare


# Format only a newly created regular image and check FAT with storage files still present.
@contextlib.contextmanager
def fat_volume(parent, size=8192, sectors=64):
    with tempfile.TemporaryDirectory(prefix='dynfilefs-fat-', dir=parent) as folder:
        root=Path(folder)
        image=root/'image.fat'
        with image.open('xb') as file:
            file.truncate(size*1024*1024)
        subprocess.run(['mkfs.fat', '-F', '32', '-s', str(sectors), str(image)], check=True, stdout=subprocess.DEVNULL)
        with image.open('rb') as file:
            header=file.read(512)
        details={'image_mib':size, 'cluster_bytes':struct.unpack_from('<H', header, 11)[0]*header[13], 'fsck':[]}
        mount=root/'fat'
        mount.mkdir()
        command=['mount', '-t', 'vfat', '-o', 'loop,noatime', str(image), str(mount)]

        # Check the cluster chains across a real FAT unmount before reopening dynfilefs.
        def remount():
            subprocess.run(['umount', str(mount)], check=True)
            result=subprocess.run(['fsck.fat', '-n', str(image)], capture_output=True, text=True)
            details['fsck'].append((result.stdout+result.stderr).replace(str(image), 'image.fat'))
            if result.returncode:
                raise RuntimeError(details['fsck'][-1])
            subprocess.run(command, check=True)

        subprocess.run(command, check=True)
        try:
            yield mount, remount, details
        finally:
            if os.path.ismount(mount):
                subprocess.run(['umount', str(mount)], check=True)
            result=subprocess.run(['fsck.fat', '-n', str(image)], capture_output=True, text=True)
            details['fsck'].append((result.stdout+result.stderr).replace(str(image), 'image.fat'))
            if result.returncode:
                raise RuntimeError(details['fsck'][-1])


if __name__=='__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('output')
    parser.add_argument('--binary', action='append', required=True, help='LABEL=/path/to/binary; may be repeated')
    parser.add_argument('--directory', required=True)
    parser.add_argument('--source-directory', required=True)
    parser.add_argument('--splits', nargs='+', type=int, default=[256, 1024, 4088])
    parser.add_argument('--repeats', type=int, default=3)
    parser.add_argument('--workers', type=int, default=8)
    args=parser.parse_args()
    binaries={label:str(Path(path).resolve()) for label, path in (value.split('=', 1) for value in args.binary)}
    parent=Path(args.directory)
    source=Path(args.source_directory)
    parent.mkdir(parents=True, exist_ok=True)
    source.mkdir(parents=True, exist_ok=True)
    cases=[(label, split) for split in args.splits for label in binaries]
    variants={f'{label}-{split}':{'binary':label, 'split_mib':split, 'samples':[]} for label, split in cases}
    with tempfile.TemporaryDirectory(prefix='dynfilefs-fat-source-', dir=source) as folder:
        large, small=prepare(Path(folder))
        for repeat in range(args.repeats):
            for label, split in cases if repeat%2==0 else reversed(cases):
                print(f'FAT32: {label}, split {split} MiB, repetition {repeat+1}', flush=True)
                with fat_volume(parent) as (fat, remount, details):
                    sample=measure(binaries[label], fat, large, small, args.workers, split, 8192, remount)
                sample['fat']=details
                variants[f'{label}-{split}']['samples'].append(sample)
                backing=subprocess.check_output(['findmnt', '-n', '-o', 'FSTYPE', '-T', str(parent)], text=True).strip()
                data={'kernel':platform.release(), 'filesystem':'XFS / dynfilefs / FAT32 / loop image', 'image_backing_filesystem':backing, 'image_mib':8192, 'virtual_mib':8192, 'cluster_bytes':32768, 'large_bytes':1024**3, 'small_files':1000, 'small_bytes':16384, 'workers':args.workers, 'repeats':args.repeats, 'variants':variants}
                Path(args.output).write_text(json.dumps(data, indent=2)+'\n')
    for variant in variants.values():
        variant['median']={key:statistics.median(sample[key] for sample in variant['samples']) for key in variant['samples'][0] if key!='fat'}
    Path(args.output).write_text(json.dumps(data, indent=2)+'\n')
