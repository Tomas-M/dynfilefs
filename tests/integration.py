#!/usr/bin/env python3
import argparse
import concurrent.futures
import contextlib
import errno
import fcntl
import os
from pathlib import Path
import random
import signal
import struct
import subprocess
import tempfile
import time


# Keep the foreground daemon observable and wait for commit before remounting.
@contextlib.contextmanager
def mounted(binary, storage, mount, size=8, split=3, stop_signal=None, mount_options=None):
    mount.mkdir(exist_ok=True)
    log_path=mount.parent/(mount.name+'.log')
    with log_path.open('wb') as log:
        command=[binary, '-d', '-f', str(storage), '-m', str(mount), '-s', str(size), '-p', str(split)]
        if mount_options:
            command += ['-o', mount_options]
        process=subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
        ready=False
        try:
            for _ in range(250):
                if process.poll() is not None or (mount/'virtual.dat').exists():
                    break
                time.sleep(0.02)
            assert process.poll() is None and (mount/'virtual.dat').exists(), log_path.read_text(errors='replace')[-4096:]
            ready=True
            yield mount/'virtual.dat'
        finally:
            if stop_signal and process.poll() is None:
                process.send_signal(stop_signal)
                process.wait(timeout=10)
            if (ready and stop_signal in (None, signal.SIGKILL)) or os.path.ismount(mount):
                subprocess.run(['umount', str(mount)], check=True)
            try:
                status=process.wait(timeout=30)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
                raise
            if stop_signal:
                assert status!=0, 'Interrupted mount exited successfully'
                assert Path(str(storage)+'.lock').exists(), 'Interrupted mount removed its recovery lock'
            else:
                assert status==0, log_path.read_text(errors='replace')[-4096:]
                assert_clean_storage(storage)


# Clean shutdown retains the durable catalog but removes temporary recovery companions.
def assert_clean_storage(storage):
    prefix=storage.name+'.'
    companions={path.name[len(storage.name):] for path in storage.parent.iterdir() if path.name.startswith(prefix) and not path.name[len(prefix):].isdigit()}
    assert companions <= {'.snapshots'}, companions


# Numeric suffixes identify data parts independently of control/metadata files.
def parts(storage):
    prefix=storage.name+'.'
    return sorted((path for path in storage.parent.iterdir() if path.name.startswith(prefix) and path.name[len(prefix):].isdigit()), key=lambda path:int(path.name[len(prefix):]))


# Compare persisted data with an independent byte-array oracle after remounting.
def check_storage(binary, root):
    storage, mount=root/'storage', root/'mount'
    expected=bytearray(8*1024*1024)
    rng=random.Random(5231)
    with mounted(binary, storage, mount) as path:
        fd=os.open(path, os.O_RDWR)
        assert not parts(storage), 'Empty storage created data parts'
        assert os.pwrite(fd, bytes(131072), 0)==131072
        os.fsync(fd)
        assert not parts(storage), 'Zero write allocated storage'
        operations=[(0, 1), (4095, 8193), (3*1024*1024-111, 131072), (len(expected)-512, 512)]
        operations.append((512*1024-1, 1024*1024+3))
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
    for offset, data in ((0, b'A'*17), (4096, b'B'*31), (512*1024+17, b'C'*31)):
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


# Unknown formats, incomplete legacy storage and damaged modern headers stay untouched.
def check_old_format(binary, root):
    for version in (400, 500, 501, 600):
        storage=root/f'old-{version}'
        header=bytearray(4096)
        struct.pack_into('<qqq', header, 2048, version, 2*1024*1024, 8*1024*1024)
        if version>=600:
            header[:8]=('DYNFS'+str(version)).encode()
            struct.pack_into('<q', header, 2048, 500)
        storage.write_bytes(header)
        split=Path(str(storage)+'.0')
        split.write_bytes(header+bytes(4096))
        before=storage.read_bytes(), split.read_bytes()
        result=subprocess.run([binary, '-f', str(storage), '-m', str(root/'mount')], capture_output=True)
        assert result.returncode!=0
        assert before==(storage.read_bytes(), split.read_bytes())
    print('PASS: unknown formats and incomplete or damaged storage rejected unchanged', flush=True)


# Retry owned conversion outputs, but never replace an unrelated existing part.
def check_automatic_conversion(binary, root):
    for state in ('new', 'retry', 'conflict', 'wrong-marker'):
        storage=root/f'auto-convert-{state}'
        header=bytearray(4096)
        struct.pack_into('<qqq', header, 2048, 500, 2*1024*1024, 2*1024*1024)
        index=bytearray(4096)
        struct.pack_into('<q', index, 13*8, 8192)
        original=header+index+bytes(7)+b'partial legacy tail'
        storage.write_bytes(header)
        old_part=Path(str(storage)+'.0')
        old_part.write_bytes(original)
        extra=Path(str(storage)+'.1')
        marker=Path(str(storage)+'.migrate')
        if state!='new':
            extra.write_bytes(b'unfinished conversion output')
        if state in ('retry', 'wrong-marker'):
            marker.write_bytes(header if state=='retry' else bytes(4096))
            Path(str(storage)+'.next').write_bytes(b'incomplete metadata')
        if state in ('conflict', 'wrong-marker'):
            result=subprocess.run([binary, '-f', str(storage), '-m', str(root/'mount')], capture_output=True)
            assert result.returncode!=0
            assert storage.read_bytes()==header
            assert extra.read_bytes()==b'unfinished conversion output'
            assert marker.exists()==(state=='wrong-marker')
        else:
            with mounted(binary, storage, root/f'auto-convert-{state}-mount', size=2) as path:
                expected=bytearray(2*1024*1024)
                expected[13*4096+7:13*4096+26]=b'partial legacy tail'
                assert path.read_bytes()==expected
                assert storage.read_bytes()[:8]==b'DYNFS600'
                assert not marker.exists()
        assert old_part.read_bytes()==original
    # Conversion must not adopt or replace an unrelated snapshot catalog.
    storage=root/'orphan-history'
    header=bytearray(4096)
    struct.pack_into('<3Q', header, 2048, 500, 2*1024*1024, 2*1024*1024)
    storage.write_bytes(header)
    history=Path(str(storage)+'.snapshots')
    history.mkdir()
    keep=history/'keep'
    keep.write_bytes(b'keep this history')
    mount=root/'orphan-mount'
    mount.mkdir()
    result=subprocess.run([binary, '-f', str(storage), '-m', str(mount)], capture_output=True)
    assert result.returncode and storage.read_bytes()==header and keep.read_bytes()==b'keep this history'
    print('PASS: automatic conversion, partial tails and interrupted-conversion retry', flush=True)


# Convert real format-500 output and preserve every original part byte for byte.
def check_conversion(binary, baseline, root):
    storage=root/'converted'
    expected=bytearray(8*1024*1024)
    with mounted(baseline, storage, root/'converted-mount', split=2) as path:
        fd=os.open(path, os.O_RDWR)
        for phase in range(3):
            offset=phase*3*1024*1024+123
            data=random.Random(phase).randbytes(4127)
            assert os.pwrite(fd, data, offset)==len(data)
            expected[offset:offset+len(data)]=data
        os.fsync(fd)
        os.close(fd)
    original={part:part.read_bytes() for part in parts(storage)}
    with mounted(binary, storage, root/'converted-mount') as path:
        assert storage.read_bytes()[:8]==b'DYNFS600'
        assert path.read_bytes()==expected
    assert all(part.read_bytes()==data for part, data in original.items())
    with mounted(binary, storage, root/'converted-mount') as path:
        fd=os.open(path, os.O_RDWR)
        assert os.pwrite(fd, b'converted overwrite', 123)==19
        expected[123:142]=b'converted overwrite'
        os.fsync(fd)
        os.close(fd)
    with mounted(binary, storage, root/'converted-mount') as path:
        assert path.read_bytes()==expected
    print('PASS: format-500 conversion preserves payload positions', flush=True)


# Huge virtual capacity must not allocate physical parts or a complete block index.
def check_lazy_storage(binary, root):
    storage=root/'sparse-64g'
    with mounted(binary, storage, root/'sparse-64g-mount', 65536, 2) as path:
        assert path.stat().st_size==64*1024**3
        assert storage.stat().st_size==4096+256*1024
        assert not parts(storage)
        fd=os.open(path, os.O_RDWR)
        assert os.pread(fd, 4096, 63*1024**3)==bytes(4096)
        assert os.pwrite(fd, b'Z'*4096, 64*1024**3-4096)==4096
        os.fsync(fd)
        os.close(fd)
        assert len(parts(storage))==1
        assert parts(storage)[0].stat().st_size <= 3*4096
    with mounted(binary, storage, root/'sparse-64g-mount', 65536, 2) as path:
        fd=os.open(path, os.O_RDONLY)
        assert os.pread(fd, 4096, 64*1024**3-4096)==b'Z'*4096
        os.close(fd)
    print('PASS: 64 GiB empty metadata, lazy parts and high-offset allocation', flush=True)


# Physical split sizes include index pages and work for odd and even MiB limits.
def check_part_limits(binary, root):
    for split in (2, 3):
        storage=root/f'limit-{split}'
        data=random.Random(split).randbytes(12*1024*1024)
        with mounted(binary, storage, root/f'limit-{split}-mount', 12, split) as path:
            fd=os.open(path, os.O_RDWR)
            assert os.pwrite(fd, data, 0)==len(data)
            os.fsync(fd)
            os.close(fd)
            assert len(parts(storage)) > 1
            assert all(part.stat().st_size <= split*1024*1024 for part in parts(storage))
        with mounted(binary, storage, root/f'limit-{split}-mount', 12, split) as path:
            assert path.read_bytes()==data
    print('PASS: physical split limits include indexes for odd/even sizes', flush=True)


# Repeated full rewrites must reuse old slots rather than grow the part address space.
def check_recycling(binary, root):
    storage, mount=root/'recycle', root/'recycle-mount'
    previous=bytes(4*1024*1024)
    for generation in range(10):
        with mounted(binary, storage, mount, 4, 2) as path:
            assert path.read_bytes()==previous
            data=bytes([generation+1])*len(previous)
            fd=os.open(path, os.O_RDWR)
            assert os.pwrite(fd, data, 0)==len(data)
            # Overwrites between checkpoints reuse their current physical copy.
            sizes=[part.stat().st_size for part in parts(storage)]
            assert os.pwrite(fd, data, 0)==len(data)
            assert [part.stat().st_size for part in parts(storage)]==sizes
            os.fsync(fd)
            for checkpoint in range(4):
                data=bytes([generation*4+checkpoint+1])*len(previous)
                assert os.pwrite(fd, data, 0)==len(data)
                os.fsync(fd)
            os.close(fd)
            assert len(parts(storage)) <= 28
            assert sum(part.stat().st_size for part in parts(storage)) <= 13*len(previous)+256*1024
            assert storage.stat().st_size < 16384
            previous=data
    with mounted(binary, storage, mount, 4, 2) as path:
        assert path.read_bytes()==previous
    print('PASS: COW between checkpoints and bounded space across ten mounts with repeated fsync', flush=True)


# Virtual growth is transactional, while a smaller requested size is ignored.
def check_growth(binary, root):
    storage, mount=root/'growth', root/'growth-mount'
    for size, expected in ((4, 4), (2, 4), (6, 6), ('+2', 8)):
        with mounted(binary, storage, mount, size, 2) as path:
            assert path.stat().st_size==expected*1024*1024
    with mounted(binary, storage, mount, '+2', 2) as path:
        assert path.stat().st_size==10*1024*1024
        fd=os.open(path, os.O_RDWR)
        assert os.pwrite(fd, b'new end', 9*1024*1024)==7
        os.close(fd)
    with mounted(binary, storage, mount, 0, 2) as path:
        assert path.stat().st_size==10*1024*1024
        assert path.read_bytes()[9*1024*1024:9*1024*1024+7]==b'new end'
    print('PASS: absolute/delta growth, ignored shrink and clean session persistence', flush=True)


# Interrupted sessions recover their latest checkpoint without creating a clean-end snapshot.
def check_crash(binary, root):
    for stop_signal in (signal.SIGKILL, signal.SIGTERM, signal.SIGINT):
        name=f'crash-{stop_signal}'
        storage, mount=root/name, root/(name+'-mount')
        expected=bytearray(8*1024*1024)
        with mounted(binary, storage, mount, split=2, stop_signal=stop_signal) as path:
            fd=os.open(path, os.O_RDWR)
            for value in (b'B', b'C'):
                data=value*(5*1024*1024)
                assert os.pwrite(fd, data, 0)==len(data)
                os.fsync(fd)
                expected[:len(data)]=data
            assert os.pwrite(fd, b'D'*4096, 0)==4096
            os.close(fd)
        assert any(Path(str(storage)+suffix).exists() for suffix in ('.log', '.work'))
        with mounted(binary, storage, mount, split=2) as path:
            assert path.read_bytes()==expected
    print('PASS: SIGKILL/SIGTERM/SIGINT retain latest successful fsync', flush=True)


# Journal rollover retains current data independently of the automatic starting point.
def check_journal(binary, root, checkpoints=8193):
    storage, mount=root/'journal', root/'journal-mount'
    expected=b'A'*4096
    with mounted(binary, storage, mount, 65536, 2, stop_signal=signal.SIGKILL) as path:
        fd=os.open(path, os.O_RDWR)
        for checkpoint in range(checkpoints):
            expected=bytes([checkpoint%251+1])*4096
            assert os.pwrite(fd, expected, 0)==4096
            os.fsync(fd)
        os.close(fd)
        log=Path(str(storage)+'.log')
        assert not log.exists() or log.stat().st_size <= 32*1024*1024
    with mounted(binary, storage, mount, 0, 2) as path:
        fd=os.open(path, os.O_RDONLY)
        assert os.pread(fd, 4096, 0)==expected
        os.close(fd)
    print(f'PASS: {checkpoints} bounded journal checkpoints and crash recovery', flush=True)


# Exercise ordinary storage operations on a disposable FAT32 backing filesystem.
def check_fat32(binary, root, lock_test=None):
    image=root/'fat32.img'
    with image.open('wb') as file:
        file.truncate(512*1024*1024)
    mount=root/'fat32'
    mount.mkdir()
    subprocess.run(['mkfs.fat', '-F', '32', '-s', '8', str(image)], check=True, stdout=subprocess.DEVNULL)
    subprocess.run(['mount', '-o', 'loop', str(image), str(mount)], check=True)
    try:
        if lock_test:
            subprocess.run([lock_test, str(mount)], check=True)
        check_lazy_storage(binary, mount)
        check_recycling(binary, mount)
        check_journal(binary, mount, 8)
    finally:
        subprocess.run(['umount', str(mount)], check=True)
    subprocess.run(['fsck.fat', '-n', str(image)], check=True)
    print('PASS: FAT32 sparse allocation, session history, recycling and filesystem check', flush=True)


# Daemonization changes cwd, so later lazy allocation must retain absolute backing paths.
def check_daemon(binary, root):
    storage, mount=root/'relative-storage', root/'daemon-mount'
    mount.mkdir()
    subprocess.run([binary, '-o', 'size=4,split=2', storage.name, mount.name], cwd=root, check=True)
    path=mount/'virtual.dat'
    try:
        for _ in range(250):
            if path.exists():
                break
            time.sleep(0.02)
        assert path.exists()
        fd=os.open(path, os.O_RDWR)
        assert os.pwrite(fd, b'daemon', 3*1024*1024)==6
        os.fsync(fd)
        os.close(fd)
    finally:
        subprocess.run(['umount', str(mount)], check=True)
    # The daemon may remove its lock before this waiter opens it.
    try:
        lock=Path(str(storage)+'.lock').open('rb')
    except FileNotFoundError:
        pass
    else:
        with lock:
            for _ in range(1000):
                try:
                    fcntl.flock(lock, fcntl.LOCK_EX|fcntl.LOCK_NB)
                    break
                except BlockingIOError:
                    time.sleep(0.01)
            else:
                raise AssertionError('Daemon did not finish its commit')
    assert_clean_storage(storage)
    with mounted(binary, storage, mount, 4, 2) as path:
        fd=os.open(path, os.O_RDONLY)
        assert os.pread(fd, 6, 3*1024*1024)==b'daemon'
        os.close(fd)
    print('PASS: fstab options, relative paths, daemonized allocation and commit', flush=True)


# Alternate same-format implementations to verify native/static interoperability.
def check_interoperability(binary, peer, root):
    storage, mount=root/'interoperable', root/'interoperable-mount'
    expected=bytearray(8*1024*1024)
    for phase, program in enumerate((binary, peer, binary)):
        with mounted(program, storage, mount) as path:
            assert path.read_bytes()==expected
            offset=phase*3*1024*1024+123
            data=random.Random(phase+812).randbytes(1048579)
            fd=os.open(path, os.O_RDWR)
            assert os.pwrite(fd, data, offset)==len(data)
            os.fsync(fd)
            os.close(fd)
            expected[offset:offset+len(data)]=data
    with mounted(peer, storage, mount) as path:
        assert path.read_bytes()==expected
        fd=os.open(path, os.O_RDWR)
        assert os.pwrite(fd, b'persisted', 123)==9
        expected[123:132]=b'persisted'
        os.close(fd)
    with mounted(binary, storage, mount) as path:
        assert path.read_bytes()==expected
    print('PASS: format-600 read/write interoperability in both directions', flush=True)


if __name__=='__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('binary')
    parser.add_argument('--baseline', help='Format-500 binary used to test automatic conversion')
    parser.add_argument('--peer', help='Another current format-600 binary used to test interoperability')
    parser.add_argument('--lock-test', help='Compiled lock test to run on the FAT32 fixture')
    parser.add_argument('--skip-loop', action='store_true', help='Skip privileged ext4/FAT32 loop mounts')
    args=parser.parse_args()
    binary=str(Path(args.binary).resolve())
    with tempfile.TemporaryDirectory(prefix='dynfilefs-test-') as folder:
        root=Path(folder)
        check_storage(binary, root)
        check_partial_tail(binary, root)
        check_concurrency(binary, root)
        check_lazy_storage(binary, root)
        check_part_limits(binary, root)
        check_recycling(binary, root)
        check_growth(binary, root)
        check_crash(binary, root)
        check_journal(binary, root)
        check_daemon(binary, root)
        international=root/'žluťoučký 文件'
        international.mkdir()
        if not args.skip_loop:
            check_loop(binary, root)
            check_fat32(binary, root, str(Path(args.lock_test).resolve()) if args.lock_test else None)
        check_old_format(binary, root)
        check_automatic_conversion(binary, root)
        if args.baseline:
            check_conversion(binary, str(Path(args.baseline).resolve()), root)
        if args.peer:
            check_interoperability(binary, str(Path(args.peer).resolve()), root)
