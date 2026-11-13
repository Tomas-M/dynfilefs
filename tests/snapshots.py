#!/usr/bin/env python3
import argparse
import contextlib
import errno
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time


MiB=1024*1024


# Keep each daemon and its disposable backing storage observable through clean shutdown.
@contextlib.contextmanager
def mounted(binary, storage, mount, size=None, restore=None, options=None):
    mount.mkdir(exist_ok=True)
    log_path=mount.parent/(mount.name+'.log')
    command=[binary, '-d', '-f', str(storage), '-m', str(mount), '-p', '3']
    if size is not None:
        command += ['-s', str(size)]
    if restore is not None:
        command += ['--restore', restore]
    if options:
        command += ['-o', options]
    with log_path.open('wb') as log:
        process=subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
        ready=False
        try:
            for _ in range(500):
                if process.poll() is not None or (mount/'virtual.dat').exists():
                    break
                time.sleep(0.02)
            assert process.poll() is None and (mount/'virtual.dat').exists(), log_path.read_text(errors='replace')[-4096:]
            ready=True
            yield mount/'virtual.dat'
        finally:
            if ready and os.path.ismount(mount):
                # Loop detach can finish its backing-file release after losetup exits.
                for _ in range(100):
                    unmount=subprocess.run(['umount', str(mount)], capture_output=True, timeout=30)
                    if unmount.returncode==0:
                        break
                    time.sleep(0.05)
                else:
                    process.kill()
                    process.wait()
                    subprocess.run(['umount', '-l', str(mount)], check=True, timeout=30)
                    raise AssertionError(unmount.stderr.decode(errors='replace'))
            try:
                status=process.wait(timeout=30)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
                raise
            assert status==0, log_path.read_text(errors='replace')[-4096:]
            assert not Path(str(storage)+'.lock').exists()


# Mount only images created beneath the test's temporary directory.
@contextlib.contextmanager
def inner_mount(image, mount, filesystem, readonly=False):
    mount.mkdir(exist_ok=True)
    options='loop'
    if readonly:
        options += ',ro,nouuid,norecovery' if filesystem=='xfs' else ',ro,noload'
    subprocess.run(['mount', '-t', filesystem, '-o', options, str(image), str(mount)], check=True, timeout=30)
    try:
        yield mount
    finally:
        subprocess.run(['umount', str(mount)], check=True, timeout=30)


# Exercise real control-file truncation and one complete UTF-8 command write.
def snapshot_command(mount, command):
    with (mount/'snapshot').open('wb', buffering=0) as control:
        data=(command+'\n').encode()
        assert control.write(data)==len(data)


# Require errors from the kernel-facing interface rather than prechecking permissions.
def expect_errno(callback, expected):
    try:
        callback()
    except OSError as error:
        assert error.errno==expected, (error.errno, expected, error)
    else:
        raise AssertionError('Expected errno '+str(expected))


# Saved views must retain their bytes while current writes and catalog changes continue.
def check_raw(binary, root):
    storage, mount=root/'raw', root/'raw-mount'
    name='čistý 系统'
    original=b'A'*4096+b'B'*4096
    with mounted(binary, storage, mount, size=8) as path:
        assert all(name.startswith('auto-') for name in (mount/'snapshot').read_text().splitlines())
        with path.open('r+b', buffering=0) as current:
            assert os.pwrite(current.fileno(), original, 4095)==len(original)
            snapshot_command(mount, 'create '+name)
            saved=mount/'snapshots'/(name+'.dat')
            assert name in (mount/'snapshot').read_text(encoding='utf-8').splitlines()
            assert name+'.dat' in {entry.name for entry in (mount/'snapshots').iterdir()}
            assert saved.stat().st_size==8*MiB
            with saved.open('rb', buffering=0) as view:
                assert os.pread(view.fileno(), len(original), 4095)==original
                assert os.pread(view.fileno(), 4096, 8*MiB)==b''
                os.pwrite(current.fileno(), b'C'*len(original), 4095)
                os.fsync(current.fileno())
                assert os.pread(view.fileno(), len(original), 4095)==original
                expect_errno(lambda: snapshot_command(mount, 'delete '+name), errno.EBUSY)
            expect_errno(lambda: os.open(saved, os.O_RDWR), errno.EROFS)
            expect_errno(lambda: os.truncate(saved, 0), errno.EROFS)
            expect_errno(lambda: os.chmod(saved, 0o600), errno.EROFS)
            snapshot_command(mount, 'create later')
        assert list(Path(str(storage)+'.snapshots').glob('*.snap'))
        expect_errno(lambda: snapshot_command(mount, 'create '+name), errno.EEXIST)
        expect_errno(lambda: snapshot_command(mount, 'delete absent'), errno.ENOENT)
        for invalid in ('create ', 'create ../escape', 'create bad/name', 'create bad\nname', 'create bad:name', 'unknown name'):
            expect_errno(lambda value=invalid: snapshot_command(mount, value), errno.EINVAL)
        # A control handle keeps one complete listing even if another point is added.
        with (mount/'snapshot').open('rb', buffering=0) as listing:
            before=(mount/'snapshot').read_bytes()
            first=listing.read(1)
            snapshot_command(mount, 'create newest')
            assert first+listing.read()==before
    with mounted(binary, storage, mount, restore=name) as path:
        assert path.stat().st_size==8*MiB
        with path.open('rb', buffering=0) as current:
            assert os.pread(current.fileno(), len(original), 4095)==original
        assert (mount/'snapshots'/'later.dat').read_bytes()[4095:4095+len(original)]==b'C'*len(original)
    with mounted(binary, storage, mount, size=12) as path:
        snapshot_command(mount, 'create larger')
        assert path.stat().st_size==12*MiB
    with mounted(binary, storage, mount, options='restore='+name) as path:
        assert path.stat().st_size==8*MiB
        assert (mount/'snapshots'/'larger.dat').stat().st_size==12*MiB
    with mounted(binary, storage, mount, size=10, restore=name) as path:
        assert path.stat().st_size==10*MiB
        before_parts={part.name:part.stat().st_size for part in storage.parent.glob(storage.name+'.[0-9]*')}
        for saved in (mount/'snapshot').read_text(encoding='utf-8').splitlines():
            snapshot_command(mount, 'delete '+saved)
        assert not list((mount/'snapshots').iterdir())
    assert Path(str(storage)+'.snapshots').is_dir()
    after_parts={part.name:part.stat().st_size for part in storage.parent.glob(storage.name+'.[0-9]*')}
    assert before_parts and after_parts==before_parts, 'Deleting snapshots changed reusable physical parts'
    print('PASS: Unicode controls, immutable views, EBUSY, errors, restore and saved sizes', flush=True)


# Long names force directory reads to resume past the first FUSE response buffer.
def check_catalog(binary, root):
    storage, mount=root/'catalog', root/'catalog-mount'
    names=[f'{i:03}-'+'snapshot-'*25 for i in range(150)]
    with mounted(binary, storage, mount, size=1):
        for name in reversed(names):
            snapshot_command(mount, 'create '+name)
        assert [name for name in (mount/'snapshot').read_text(encoding='utf-8').splitlines() if not name.startswith('auto-')]==list(reversed(names))
        assert sorted(entry.name for entry in (mount/'snapshots').iterdir() if not entry.name.startswith('auto-'))==[name+'.dat' for name in names]
    with mounted(binary, storage, mount):
        assert [name for name in (mount/'snapshot').read_text(encoding='utf-8').splitlines() if not name.startswith('auto-')]==list(reversed(names))
        for name in names:
            snapshot_command(mount, 'delete '+name)
        assert all(name.startswith('auto-') for name in (mount/'snapshot').read_text().splitlines())
    print('PASS: paginated snapshot directory and durable catalog with many names', flush=True)


# Live unsynced inner files must be captured by automatic freeze, then remain independent.
def check_filesystem(binary, root, filesystem):
    formatter=shutil.which('mkfs.'+filesystem)
    if formatter is None:
        print('SKIP: mkfs.'+filesystem+' unavailable', flush=True)
        return
    storage, mount=root/filesystem, root/(filesystem+'-mount')
    live, old=root/(filesystem+'-live'), root/(filesystem+'-old')
    content=(b'captured before freeze\n'*2048)+bytes(range(256))
    size=384 if filesystem=='xfs' else 32
    with mounted(binary, storage, mount, size=size) as path:
        arguments=[formatter, '-f', str(path)] if filesystem=='xfs' else [formatter, '-q', '-F', str(path)]
        subprocess.run(arguments, check=True, stdout=subprocess.DEVNULL, timeout=60)
        with inner_mount(path, live, filesystem):
            (live/'kept.txt').write_bytes(content)
            snapshot_command(mount, 'create frozen')
            # A timed external writer makes failed thaw observable without hanging this test.
            subprocess.run(['python3', '-c', 'from pathlib import Path; import sys; Path(sys.argv[1]).write_bytes(b"after snapshot")', str(live/'kept.txt')], check=True, timeout=15)
            (live/'later.txt').write_text('not in the saved point')
            image=mount/'snapshots'/'frozen.dat'
            with inner_mount(image, old, filesystem, readonly=True):
                assert (old/'kept.txt').read_bytes()==content
                assert not (old/'later.txt').exists()
                expect_errno(lambda: snapshot_command(mount, 'delete frozen'), errno.EBUSY)
            assert (live/'kept.txt').read_bytes()==b'after snapshot'
    with mounted(binary, storage, mount, restore='frozen') as path:
        with inner_mount(path, live, filesystem):
            assert (live/'kept.txt').read_bytes()==content
            assert not (live/'later.txt').exists()
    print('PASS: '+filesystem+' auto-freeze, thaw, simultaneous RO snapshot and restore', flush=True)


# A detected but unmounted loop must not be mistaken for an unused raw image.
def check_unmounted_loop(binary, root):
    storage, mount=root/'unmounted-loop', root/'unmounted-loop-mount'
    with mounted(binary, storage, mount, size=8) as path:
        loop=subprocess.check_output(['losetup', '--find', '--show', str(path)], text=True).strip()
        try:
            expect_errno(lambda: snapshot_command(mount, 'create unsafe'), errno.EBUSY)
            assert all(name.startswith('auto-') for name in (mount/'snapshot').read_text().splitlines())
        finally:
            subprocess.run(['losetup', '-d', loop], check=True, timeout=15)
    print('PASS: matching unmounted loop rejects automatic snapshot creation', flush=True)


# Compare content and directory metadata so read-only inspection cannot hide writes.
def disk_state(root):
    result={}
    for path in sorted(root.rglob('*')):
        stat=path.stat()
        result[str(path.relative_to(root))]=(stat.st_mode, stat.st_size, stat.st_mtime_ns, stat.st_ctime_ns, hashlib.sha256(path.read_bytes()).digest() if path.is_file() else None)
    return result


# Lineage uses stable identities rather than mutable or reused display names.
def check_history(binary, root):
    folder=root/'history-fixture'
    folder.mkdir()
    storage, mount=folder/'storage', folder/'mount'
    name='A český 系统'
    with mounted(binary, storage, mount, size=8) as path:
        initial=json.loads((mount/'snapshots.json').read_text())
        assert len(initial['snapshots'])==1 and initial['snapshots'][0]['automatic']
        assert initial['currentParent']==initial['snapshots'][0]['id']
        with path.open('r+b', buffering=0) as current:
            for point, byte in ((name, b'A'), ('B', b'B'), ('C', b'C')):
                assert os.pwrite(current.fileno(), byte*4096, 0)==4096
                snapshot_command(mount, 'create '+point)
        history=json.loads((mount/'snapshots.json').read_text())
        points={point['name']:point for point in history['snapshots']}
        a, b, c=(points[value] for value in (name, 'B', 'C'))
        assert b['parent']==a['id'] and c['parent']==b['id'] and history['currentParent']==c['id']
        assert all(len(point['id'])==16 for point in points.values())
        stats=json.loads((mount/'stats').read_text())
        assert stats['version']==600 and stats['size']==8*MiB and stats['currentParent']==c['id']
        assert stats['reusableBytes']>=0 and stats['reusableBytes']%4096==0
        # An external inspection cannot account for unsynced allocations in a running daemon.
        for option in ('--info', '--snapshots'):
            result=subprocess.run([binary, '-f', str(storage), option, '--json'], capture_output=True)
            assert result.returncode and not result.stdout
    with mounted(binary, storage, mount, restore='B') as path:
        assert path.read_bytes()[:4096]==b'B'*4096
        with path.open('r+b', buffering=0) as current:
            assert os.pwrite(current.fileno(), b'D'*4096, 0)==4096
            snapshot_command(mount, 'create D')
        history=json.loads((mount/'snapshots.json').read_text())
        points={point['name']:point for point in history['snapshots']}
        d=points['D']
        assert points['C']['parent']==b['id'] and d['parent']==b['id']
        assert history['currentParent']==d['id']
        before=json.loads((mount/'stats').read_text())['reusableBytes']
        snapshot_command(mount, 'delete B')
        after=json.loads((mount/'stats').read_text())['reusableBytes']
        assert after>=before+4096
        history=json.loads((mount/'snapshots.json').read_text())
        deleted=next(point for point in history['snapshots'] if point['id']==b['id'])
        assert deleted['deleted'] and deleted['parent']==a['id']
        assert Path(str(storage)+'.snapshots', b['id']+'.snap').stat().st_size==4096
        snapshot_command(mount, 'create B')
        history=json.loads((mount/'snapshots.json').read_text())
        reused=[point for point in history['snapshots'] if point['name']=='B']
        assert len(reused)==2 and len({point['id'] for point in reused})==2
        assert sum(point['deleted'] for point in reused)==1
    before=disk_state(folder)
    output=subprocess.check_output([binary, '-f', str(storage), '--snapshots', '--json'], text=True)
    saved=json.loads(output)
    assert saved==history
    tree=subprocess.check_output([binary, '-f', str(storage), '--snapshots'], text=True)
    assert all(point in tree for point in (name, 'B', 'C', 'D')) and b['id'] in tree
    info=json.loads(subprocess.check_output([binary, '-f', str(storage), '--info', '--json'], text=True))
    assert info['currentParent']==saved['currentParent'] and info['reusableBytes']%4096==0
    assert disk_state(folder)==before
    print('PASS: offline/live JSON lineage, branching, deleted ancestors, reused names and reusableBytes', flush=True)


if __name__=='__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('binary')
    parser.add_argument('--skip-loop', action='store_true', help='Skip privileged ext4/XFS and unmounted-loop checks')
    args=parser.parse_args()
    binary=str(Path(args.binary).resolve())
    with tempfile.TemporaryDirectory(prefix='dynfilefs-snapshots-') as folder:
        root=Path(folder)
        check_raw(binary, root)
        check_catalog(binary, root)
        check_history(binary, root)
        if not args.skip_loop:
            check_filesystem(binary, root, 'ext4')
            check_filesystem(binary, root, 'xfs')
            check_unmounted_loop(binary, root)
