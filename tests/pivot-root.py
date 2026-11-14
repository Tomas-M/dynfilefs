#!/usr/bin/env python3
import argparse
import ctypes
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

from snapshots import inner_mount, mounted, snapshot_command


MiB=1024*1024


# Runtime writes must follow the owned directory even after its former name is reused.
def check_rename(binary, root):
    original=root/'original'
    original.mkdir()
    moved=root/'relocated'
    storage=original/'changes.dat'
    with mounted(binary, storage, root/'raw-mount', size=16) as image:
        with image.open('r+b', buffering=0) as disk:
            os.pwrite(disk.fileno(), b'A'*(4*MiB), 0)
            os.fsync(disk.fileno())
            snapshot_command(image.parent, 'create before move')
            before=len(list(original.glob('changes.dat.[0-9]*')))
            original.rename(moved)
            original.mkdir()
            os.pwrite(disk.fileno(), b'B'*(4*MiB), 4*MiB)
            os.fsync(disk.fileno())
            snapshot_command(image.parent, 'create after move')
            snapshot_command(image.parent, 'delete before move')
            assert len(list(moved.glob('changes.dat.[0-9]*')))>before
            assert not list(original.iterdir()), 'A runtime operation followed the reused pathname'
    storage=moved/'changes.dat'
    assert not Path(str(storage)+'.lock').exists()
    assert not any(Path(str(storage)+suffix).exists() for suffix in ('.work','.log','.next'))
    with mounted(binary, storage, root/'raw-mount', restore='after move') as image:
        with image.open('rb', buffering=0) as disk:
            assert os.pread(disk.fileno(), 4*MiB, 0)==b'A'*(4*MiB)
            assert os.pread(disk.fileno(), 4*MiB, 4*MiB)==b'B'*(4*MiB)
    print('PASS: renamed backing directory, reused old name, new parts, journal, snapshots and lock cleanup', flush=True)


# Compile the host's syscall number instead of assuming a particular test architecture.
def pivot_helper(root):
    source=root/'pivot.c'
    source.write_text('#include <unistd.h>\n#include <sys/syscall.h>\n#include <sys/mount.h>\nint pivot(const char *root,const char *old) { return syscall(SYS_pivot_root,root,old); }\nint drop_mount(const char *path) { return umount2(path,0); }\n')
    library=root/'pivot.so'
    subprocess.run(['cc','-shared','-fPIC','-o',str(library),str(source)],check=True)
    helper=ctypes.CDLL(str(library),use_errno=True)
    helper.pivot.argtypes=[ctypes.c_char_p,ctypes.c_char_p]
    helper.drop_mount.argtypes=[ctypes.c_char_p]
    return helper


# The daemon survives a real root transition; freeze must discover its relocated loop consumer.
def check_pivot(binary, root, filesystem):
    root=root/filesystem
    root.mkdir()
    helper=pivot_helper(root)
    storage=root/'changes.dat'
    fuse=root/'fuse'
    live=root/'live'
    newroot=root/'newroot'
    newroot.mkdir()
    subprocess.run(['mount','-t','tmpfs','tmpfs',str(newroot)],check=True)
    for name in ('oldroot','proc','sys','dev'):(newroot/name).mkdir()
    for name in ('proc','sys','dev'):
        subprocess.run(['mount','--bind','/'+name,str(newroot/name)],check=True)
    content=b'Captured after pivot \xc4\x8d\xe6\x96\x87\xe4\xbb\xb6\n'*(6*MiB//30)
    point='after pivot český 文件'
    try:
        with mounted(binary,storage,fuse,size=384 if filesystem=='xfs' else 64) as image:
            formatter=['mkfs.xfs','-f',str(image)] if filesystem=='xfs' else ['mkfs.ext4','-q','-F',str(image)]
            subprocess.run(formatter,check=True,stdout=subprocess.DEVNULL,timeout=60)
            live.mkdir(exist_ok=True)
            subprocess.run(['mount','-t',filesystem,'-o','loop',str(image),str(live)],check=True,timeout=30)
            try:
                (live/'before.txt').write_text('before pivot\n')
                snapshot_command(fuse,'create before pivot')
                before=len(list(root.glob('changes.dat.[0-9]*')))
                oldroot=os.open('/',os.O_RDONLY|os.O_DIRECTORY)
                cwd=os.getcwd()
                assert helper.pivot(os.fsencode(newroot),os.fsencode(newroot/'oldroot'))==0,os.strerror(ctypes.get_errno())
                shifted=Path('/oldroot')/root.relative_to('/')
                try:
                    assert not storage.exists(), 'Original absolute backing path is still visible'
                    with (shifted/'live'/'kept.txt').open('wb') as output:
                        output.write(content)
                        output.flush()
                        os.fsync(output.fileno())
                    assert len(list(shifted.glob('changes.dat.[0-9]*')))>before
                    # Leave this last write dirty: snapshot creation must freeze the actual inner FS.
                    (shifted/'live'/'unsynced.txt').write_text('captured by freeze\n')
                    snapshot_command(shifted/'fuse','create '+point)
                    snapshot_command(shifted/'fuse','delete before pivot')
                    with (shifted/'live'/'kept.txt').open('wb') as output:
                        output.write(b'changed after snapshot\n')
                        output.flush()
                        os.fsync(output.fileno())
                    (shifted/'live'/'later.txt').write_text('after snapshot\n')
                    history=json.loads((shifted/'fuse'/'snapshots.json').read_text())
                    assert any(item['name']=='before pivot' and item['deleted'] for item in history['snapshots'])
                    assert any(item['name']==point and not item['deleted'] for item in history['snapshots'])
                    # Final publication and lock cleanup must work before the old root becomes / again.
                    assert helper.drop_mount(os.fsencode(shifted/'live'))==0,os.strerror(ctypes.get_errno())
                    for _ in range(100):
                        if helper.drop_mount(os.fsencode(shifted/'fuse'))==0:break
                        time.sleep(.05)
                    else:raise OSError(ctypes.get_errno(),os.strerror(ctypes.get_errno()))
                    for _ in range(500):
                        if not (shifted/'changes.dat.lock').exists():break
                        time.sleep(.02)
                    assert not any((shifted/('changes.dat'+suffix)).exists() for suffix in ('.lock','.work','.log','.next'))
                finally:
                    os.fchdir(oldroot)
                    assert helper.pivot(b'.',os.fsencode(str(newroot).lstrip('/')))==0,os.strerror(ctypes.get_errno())
                    os.close(oldroot)
                    os.chdir(cwd)
            finally:
                if os.path.ismount(live):subprocess.run(['umount',str(live)],check=True,timeout=30)
        with mounted(binary,storage,fuse) as image:
            with inner_mount(fuse/'snapshots'/(point+'.dat'),root/'saved',filesystem,readonly=True) as saved:
                assert (saved/'kept.txt').read_bytes()==content
                assert (saved/'unsynced.txt').read_text()=='captured by freeze\n'
                assert not (saved/'later.txt').exists()
        assert not any(Path(str(storage)+suffix).exists() for suffix in ('.lock','.work','.log','.next'))
        with mounted(binary,storage,fuse,restore=point) as image:
            with inner_mount(image,live,filesystem):
                assert (live/'kept.txt').read_bytes()==content
                assert (live/'unsynced.txt').read_text()=='captured by freeze\n'
                assert not (live/'later.txt').exists()
        print('PASS: '+filesystem+' pivot_root, new parts, fsync, freeze/thaw, create/delete, immutable snapshot, restore and clean close',flush=True)
    finally:
        for name in ('dev','sys','proc'):
            subprocess.run(['umount',str(newroot/name)],check=True)
        subprocess.run(['umount',str(newroot)],check=True)


if __name__=='__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('binary')
    parser.add_argument('--only-pivot',action='store_true')
    parser.add_argument('--namespace',action='store_true',help=argparse.SUPPRESS)
    args=parser.parse_args()
    binary=str(Path(args.binary).resolve())
    if not args.namespace:
        # PID namespace teardown also stops a failed or frozen test daemon.
        command=['unshare','--mount','--propagation','private','--pid','--fork','--kill-child',sys.executable,str(Path(__file__).resolve()),binary,'--namespace']
        if args.only_pivot:command.append('--only-pivot')
        raise SystemExit(subprocess.run(command,timeout=180).returncode)
    assert os.geteuid()==0,'This regression needs root for isolated mounts'
    root=Path(tempfile.mkdtemp(prefix='dynfilefs-pivot-'))
    print('Test workspace: '+str(root),flush=True)
    # Failed runs retain diagnostics; never recursively delete a tree during failed mount cleanup.
    if not args.only_pivot:check_rename(binary,root)
    check_pivot(binary,root,'xfs')
    check_pivot(binary,root,'ext4')
    shutil.rmtree(root)
