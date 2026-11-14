#!/usr/bin/env python3
import argparse
import fcntl
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tempfile


MiB=1024*1024


# Encode independent disk fixtures rather than asking the implementation to write its own oracle.
def checksum(data):
    value=14695981039346656037
    for byte in data:
        value=((value^byte)*1099511628211)&((1<<64)-1)
    return value


# Format600 roots begin at4096 for both32-bit and64-bit writers.
def snapshot(size, split=2*MiB, sequence=0, point=0, parent=0, name=''):
    roots=(size+2*MiB-1)//(2*MiB)
    data=bytearray(4096+8*roots)
    struct.pack_into('<8s9Q', data, 0, b'DYNFS600', 600, size, split, 0, 0, sequence, point, parent, 1700000000 if point else 0)
    encoded=name.encode()
    data[80:80+len(encoded)]=encoded
    struct.pack_into('<Q', data, 40, checksum(data))
    return data


# Parent changes are journaled even if geometry and every block reference stay unchanged.
def record(size, sequence, parent=0):
    data=bytearray(4096)
    struct.pack_into('<8s8Q', data, 0, b'DYNLOG02', 4096, 0, sequence, size, 0, 0, 0, parent)
    struct.pack_into('<Q', data, 16, checksum(data))
    return data


# Reads may change atime; persisted contents, names, modes, mtime and ctime must stay fixed.
def tree_state(root):
    result={}
    for path in sorted(root.rglob('*')):
        stat=path.stat()
        result[str(path.relative_to(root))]=(stat.st_mode, stat.st_size, stat.st_mtime_ns, stat.st_ctime_ns, hashlib.sha256(path.read_bytes()).digest() if path.is_file() else None)
    return result


# Inspect never mounts, creates a lock, publishes recovery or changes any companion.
def inspect(binary, root, arguments, expected=None, json_output=False):
    before=tree_state(root)
    result=subprocess.run([binary, *arguments], capture_output=True, text=True)
    assert tree_state(root)==before
    if expected is None:
        assert result.returncode and not result.stdout, result
    else:
        assert result.returncode==0 and not result.stderr, result
        actual=json.loads(result.stdout) if json_output else dict(line.split(':', 1) for line in result.stdout.splitlines())
        assert actual==expected, (actual, expected)
    return result


# Every value has one documented unit; JSON parent IDs remain safe in JavaScript.
def fields(size, split=2*MiB, version=600, reusable=0, parent=None, json_output=False):
    result={'version':version, 'size':size, 'sizeMiB':size//MiB, 'split':split, 'splitMiB':split//MiB, 'blockSize':4096, 'reusableBytes':reusable, 'currentParent':parent}
    return result if json_output else {key:str(value) if value is not None else 'none' for key,value in result.items()}


# Latest saved geometry and lineage must be read without applying the recovery path.
def check_info(binary, root):
    storage=root/'store'
    storage.write_bytes(snapshot(4*MiB))
    part=Path(str(storage)+'.0')
    part.write_bytes(bytes(3*4096))
    expected=fields(4*MiB, reusable=3*4096)
    inspect(binary, root, ['-i', '-f', str(storage)], expected)
    inspect(binary, root, ['--info', '--json', str(storage)], fields(4*MiB, reusable=3*4096, json_output=True), True)
    log=Path(str(storage)+'.log')
    log.write_bytes(record(6*MiB, 1)+record(8*MiB, 2))
    inspect(binary, root, ['-i', str(storage)], fields(8*MiB, reusable=3*4096))
    work=Path(str(storage)+'.work')
    work.write_bytes(snapshot(8*MiB, sequence=2))
    log.write_bytes(record(10*MiB, 3)+record(12*MiB, 4)[:131])
    inspect(binary, root, ['-i', str(storage)], fields(10*MiB, reusable=3*4096))
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
    # Unknown modern formats are never interpreted as500.
    for version in (599, 601):
        damaged=snapshot(4*MiB)
        damaged[:8]=('DYNFS'+str(version)).encode()
        struct.pack_into('<Q', damaged, 8, version)
        struct.pack_into('<3Q', damaged, 2048, 500, 2*MiB, 4*MiB)
        storage.write_bytes(damaged)
        inspect(binary, root, ['-i', str(storage)])
    storage.write_bytes(snapshot(4*MiB))
    lock=Path(str(storage)+'.lock')
    with lock.open('wb') as held:
        fcntl.flock(held, fcntl.LOCK_EX|fcntl.LOCK_NB)
        inspect(binary, root, ['-i', str(storage)])
        inspect(binary, root, ['--snapshots', '--json', str(storage)])
    lock.unlink()
    for extra in (['-m', str(root/'mount')], ['-s', '8'], ['-p', '0'], ['-o', 'size=8'], ['-d'], ['--restore', 'A'], ['--unknown']):
        inspect(binary, root, ['-i', '-f', str(storage), *extra])
    inspect(binary, root, ['-i', str(root/'missing')])
    print('PASS: read-only600 inspection, exact reusable slots, JSON, saved recovery state and errors', flush=True)


# Deleted ancestors survive name reuse and JSON serialization must preserve international text.
def check_tree(binary, root):
    storage=root/'tree'
    directory=Path(str(storage)+'.snapshots')
    directory.mkdir()
    names=['A čistý 系统', 'B "quoted"\\name\n', 'C', 'B "quoted"\\name\n']
    parents=[0, 1, 2, 1]
    for point, (name,parent) in enumerate(zip(names, parents), 1):
        data=snapshot(0 if point==2 else 4*MiB, point=point, parent=parent, name=name)
        (directory/(f'{point:016x}.snap')).write_bytes(data)
    storage.write_bytes(snapshot(4*MiB, parent=4))
    expected={'currentParent':'0000000000000004', 'snapshots':[
        {'id':f'{point:016x}', 'parent':f'{parent:016x}' if parent else None, 'name':name, 'created':1700000000, 'deleted':point==2, 'automatic':False, 'protected':False}
        for point,(name,parent) in enumerate(zip(names, parents),1)
    ]}
    inspect(binary, root, ['--snapshots', '--json', '-f', str(storage)], expected, True)
    before=tree_state(root)
    text=subprocess.check_output([binary, '--snapshots', '-f', str(storage)], text=True)
    assert 'A čistý 系统' in text and '0000000000000004' in text and tree_state(root)==before
    # An unknown ancestry link must not silently produce a misleading tree.
    (directory/'0000000000000014.snap').write_bytes(snapshot(4*MiB, point=20, parent=17, name='orphan'))
    inspect(binary, root, ['--snapshots', '--json', '-f', str(storage)])
    (directory/'0000000000000014.snap').unlink()
    (directory/'0000000000000003.snap').write_bytes(snapshot(4*MiB, point=3, parent=0, name='C'))
    storage.write_bytes(snapshot(4*MiB, parent=99))
    inspect(binary, root, ['--snapshots', '--json', '-f', str(storage)])
    print('PASS: offline lineage, history-only nodes, repeated display names and JSON escaping', flush=True)


# Released format500 can still be inspected without conversion or new metadata.
def check_legacy(binary, root):
    storage=root/'legacy'
    data=bytearray(4096)
    struct.pack_into('<3Q', data, 2048, 500, 3*MiB, 7*MiB)
    storage.write_bytes(data)
    inspect(binary, root, ['--info', '-f', str(storage)], fields(7*MiB, 3*MiB, version=500))
    inspect(binary, root, ['--snapshots', '--json', '-f', str(storage)], {'currentParent':None, 'snapshots':[]}, True)
    storage.write_bytes(data[:2072])
    inspect(binary, root, ['-i', str(storage)])
    print('PASS: legacy read-only geometry and empty history without conversion', flush=True)


if __name__=='__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('binary')
    args=parser.parse_args()
    binary=str(Path(args.binary).resolve())
    with tempfile.TemporaryDirectory(prefix='dynfilefs-info-') as folder:
        root=Path(folder)
        check_info(binary, root)
        check_tree(binary, root)
        check_legacy(binary, root)
