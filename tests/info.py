#!/usr/bin/env python3
import argparse
import hashlib
from pathlib import Path
import struct
import subprocess
import tempfile


MiB=1024*1024


# Build independent fixtures using the documented format checksum.
def checksum(data):
    value=14695981039346656037
    for byte in data:
        value=((value^byte)*1099511628211)&0xffffffffffffffff
    return value


# An empty snapshot needs root entries but no allocation bitmap or data parts.
def snapshot(size, split=2*MiB, sequence=0):
    data=bytearray(4096+((size-1)//(2*MiB)+1)*8)
    struct.pack_into('<8s6Q', data, 0, b'DYNFS600', 600, size, split, 0, 0, sequence)
    struct.pack_into('<Q', data, 40, checksum(data))
    return data


# Geometry-only checkpoints grow the virtual disk without allocating data.
def record(size, sequence):
    data=bytearray(4096)
    struct.pack_into('<8s7Q', data, 0, b'DYNLOG01', 4096, 0, sequence, size, 0, 0, 0)
    struct.pack_into('<Q', data, 16, checksum(data))
    return data


# Include directory timestamps to detect even temporary file creation/removal.
def tree_state(root):
    state={}
    for path in [root, *root.rglob('*')]:
        stat=path.stat()
        digest=hashlib.sha256(path.read_bytes()).digest() if path.is_file() else None
        state[str(path.relative_to(root))]=(stat.st_mode, stat.st_mtime_ns, stat.st_ctime_ns, digest)
    return state


# Every invocation, including errors, must leave backing files untouched.
def inspect(binary, root, arguments, expected=None):
    before=tree_state(root)
    result=subprocess.run([binary, *arguments], capture_output=True, text=True)
    assert tree_state(root)==before, 'Info command changed storage files or directory metadata'
    if expected is None:
        assert result.returncode!=0 and not result.stdout and result.stderr, result
        return
    assert result.returncode==0 and not result.stderr, result
    lines=result.stdout.splitlines()
    assert result.stdout.endswith('\n') and len(lines)==len(expected), result.stdout
    values=dict(line.split(':') for line in lines)
    assert values==expected, (values, expected)


# Expected fields are a public CLI contract rather than implementation internals.
def fields(size, split=2*MiB, version=600, rollback=0):
    result={'version':str(version), 'blockSize':'4096', 'rollback':str(rollback)}
    for name, value in (('size', size), ('split', split)):
        result[name]=str(value)
        result[name+'MiB']=str(value//MiB)
    return result


# Exercise inspection without FUSE privileges or a writable backing mount.
def check_info(binary, root):
    storage=root/'žluťoučký 文件'
    storage.write_bytes(snapshot(4*MiB))
    for arguments in (['-i', '-f', str(storage)], ['--info', str(storage)], ['-i', str(storage)]):
        inspect(binary, root, arguments, fields(4*MiB))

    storage.write_bytes(snapshot(4*MiB+4096, 2*MiB+8192))
    inspect(binary, root, ['-i', str(storage)], fields(4*MiB+4096, 2*MiB+8192))

    # Pending rollback must not select the original smaller geometry or clear its marker.
    storage.write_bytes(snapshot(4*MiB))
    log=Path(str(storage)+'.log')
    log.write_bytes(record(6*MiB, 1)+record(8*MiB, 2))
    marker=Path(str(storage)+'.rollback')
    marker.touch()
    Path(str(storage)+'.0').write_bytes(b'payload must stay untouched')
    Path(str(storage)+'.next').write_bytes(b'unpublished candidate')
    inspect(binary, root, ['-i', str(storage)], fields(8*MiB, rollback=1))

    work=Path(str(storage)+'.work')
    work.write_bytes(snapshot(8*MiB, sequence=2))
    log.write_bytes(record(10*MiB, 3)+record(12*MiB, 4)[:131])
    inspect(binary, root, ['-i', str(storage)], fields(10*MiB, rollback=1))
    marker.unlink()
    inspect(binary, root, ['-i', str(storage)], fields(10*MiB))

    # A stable corrupt checkpoint or interior log record must fail without partial output.
    damaged=record(10*MiB, 3)
    damaged[100]^=1
    log.write_bytes(damaged+record(12*MiB, 4))
    inspect(binary, root, ['-i', str(storage)])
    log.unlink()
    work.write_bytes(b'incomplete checkpoint')
    inspect(binary, root, ['-i', str(storage)])
    work.unlink()
    damaged=snapshot(4*MiB)
    damaged[-1]^=1
    storage.write_bytes(damaged)
    inspect(binary, root, ['-i', str(storage)])
    damaged=snapshot(4*MiB)
    struct.pack_into('<Q', damaged, 8, 601)
    struct.pack_into('<3Q', damaged, 2048, 500, 2*MiB, 4*MiB)
    storage.write_bytes(damaged)
    inspect(binary, root, ['-i', str(storage)])

    # Legacy inspection reads its header, including odd splits, without converting anything.
    legacy=root/'legacy'
    data=bytearray(4096)
    struct.pack_into('<3Q', data, 2048, 500, 3*MiB, 7*MiB)
    legacy.write_bytes(data)
    inspect(binary, root, ['--info', '-f', str(legacy)], fields(7*MiB, 3*MiB, version=500))
    legacy.write_bytes(data[:2072])
    inspect(binary, root, ['-i', str(legacy)])
    struct.pack_into('<Q', data, 2048, 501)
    legacy.write_bytes(data)
    inspect(binary, root, ['-i', str(legacy)])
    legacy.write_bytes(data[:2050])
    inspect(binary, root, ['-i', str(legacy)])
    inspect(binary, root, ['-i', str(root/'missing')])

    storage.write_bytes(snapshot(4*MiB))
    for extra in (['-m', str(root/'mount')], ['-s', '8'], ['-s', '0'], ['-p', '0'], ['-o', 'norollback'], ['-d'], ['--norollback'], ['--unknown']):
        inspect(binary, root, ['-i', '-f', str(storage), *extra])
    print('PASS: read-only info, CLI fields, legacy format, checkpoint/log recovery, rollback and errors', flush=True)


if __name__=='__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('binary')
    args=parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='dynfilefs-info-') as folder:
        check_info(str(Path(args.binary).resolve()), Path(folder))
