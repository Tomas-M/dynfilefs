#!/usr/bin/env python3
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from benchmarks.fat import fat_volume
from benchmarks.run import mounted as traced_mount
from integration import mounted


# A large optional reservation must not prevent a smaller write that still fits.
def check_pressure(binary, parent, trace):
    payload=bytes(range(256))*256
    with fat_volume(parent, 128, 1) as (fat, remount, details):
        root=fat/'pressure'
        root.mkdir()
        with traced_mount(binary, root, trace, summary=False) as (path, pid):
            free=os.statvfs(fat).f_bavail*os.statvfs(fat).f_frsize
            remaining=free-1024*1024
            with (fat/'filler').open('wb') as file:
                chunk=b'F'*1024*1024
                while remaining:
                    size=min(remaining, len(chunk))
                    file.write(chunk[:size])
                    remaining-=size
                file.flush()
                os.fsync(file.fileno())
            fd=os.open(path, os.O_RDWR)
            assert os.pwrite(fd, payload, 0)==len(payload)
            os.fsync(fd)
            assert os.pread(fd, len(payload), 0)==payload
            os.close(fd)
        assert any('fallocate(' in line and 'ENOSPC' in line for line in trace.read_text().splitlines()), trace.read_text()
        remount()
        with mounted(binary, root/'storage', root/'mount', 512, 32) as path:
            fd=os.open(path, os.O_RDONLY)
            assert os.pread(fd, len(payload), 0)==payload
            os.close(fd)
    print('PASS: FAT32 reservation ENOSPC with successful smaller write, FAT fsck and remount', flush=True)
    return details


if __name__=='__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('binary')
    parser.add_argument('--directory', required=True)
    parser.add_argument('--baseline', required=True)
    parser.add_argument('--output', required=True)
    args=parser.parse_args()
    binary=str(Path(args.binary).resolve())
    parent=Path(args.directory)
    parent.mkdir(parents=True, exist_ok=True)
    with fat_volume(parent, 1024, 8) as (fat, remount, integration_details):
        subprocess.run([sys.executable, 'tests/integration.py', binary, '--baseline', str(Path(args.baseline).resolve())], env={**os.environ, 'TMPDIR':str(fat)}, check=True)
        remount()
    pressure_details=check_pressure(binary, parent, Path(args.output).with_suffix('.strace').resolve())
    Path(args.output).write_text(json.dumps({'integration':integration_details, 'pressure':pressure_details}, indent=2)+'\n')
