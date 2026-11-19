#!/usr/bin/env python3
import argparse
from pathlib import Path
import subprocess,tempfile
from info import MiB,snapshot,tree_state


# Independent format-600 fixtures include deleted and hidden forks, without a mounted filesystem.
def fixture(root,records,parent):
    storage=root/'changes.dat';directory=root/'changes.dat.snapshots';directory.mkdir()
    for point,ancestor,name,deleted in records:
        (directory/(f'{point:016x}.snap')).write_bytes(snapshot(0 if deleted else 4*MiB,point=point,parent=ancestor,name=name))
    storage.write_bytes(snapshot(4*MiB,parent=parent));return storage


# Rendering is read-only and every selectable row retains the original exact identity.
def report(binary,storage,*hidden,success=True):
    before=tree_state(storage.parent);args=[binary,'--snapshots','--tsv','-f',str(storage)]
    for entry in hidden:args+=['--hide',entry]
    result=subprocess.run(args,capture_output=True,text=True)
    assert tree_state(storage.parent)==before
    if not success:assert result.returncode and not result.stdout;return
    assert not result.returncode and not result.stderr,result
    rows=[line.split('\t') for line in result.stdout.splitlines()]
    assert all(len(row)==5 and row[2]=='2023-11-14 22:13:20 UTC' for row in rows)
    return [(int(row[0],16),row[1],int(row[3]),int(row[4],16) if row[4] else 0) for row in rows]


# Current-path ordering and compact lanes must survive collapsing consecutive hidden ancestors.
def check_branches(binary,root):
    records=[(1,0,'unformatted',False),(2,1,'Initial state',False),(3,2,'owned guard',False),
        (4,3,'český 文件',False),(5,4,'deleted branch',True),(6,5,'side',False),(7,4,'next',False),(8,7,'auto-example',False)]
    storage=fixture(root,records,8);hidden=['0000000000000001:unformatted','0000000000000003:owned guard']
    assert report(binary,storage,*hidden)==[(2,'Initial state',0,0),(4,'český 文件',0,2),(6,'side',1,4),(7,'next',0,4),(8,'auto-example',0,7)]
    stale=report(binary,storage,hidden[0],'0000000000000003:unrelated name')
    assert (3,'owned guard',0,2) in stale
    (root/'changes.dat.snapshots/000000000000000a.snap').write_bytes(snapshot(4*MiB,point=10,parent=5,name='second side'))
    assert report(binary,storage,*hidden)==[(2,'Initial state',0,0),(4,'český 文件',0,2),(6,'side',1,4),(10,'second side',1,4),(7,'next',0,4),(8,'auto-example',0,7)]
    storage.write_bytes(snapshot(4*MiB,parent=6))
    assert [row[0] for row in report(binary,storage,*hidden)]==[2,4,7,8,10,6]
    assert report(binary,storage,*hidden,'0000000000000004:český 文件')==[(2,'Initial state',0,0),(7,'next',1,2),(8,'auto-example',1,7),(10,'second side',1,2),(6,'side',0,2)]
    print('PASS: selectable tree rows, exact ownership filters, deleted forks, Unicode and current-branch ordering',flush=True)


# A long linear history remains one lane and does not require recursive traversal.
def check_linear(binary,root):
    records=[(i,i-1,'point '+str(i),False) for i in range(1,1025)]
    storage=fixture(root,records,1024);rows=report(binary,storage)
    assert len(rows)==1024 and all(row[2]==0 and row[3]==row[0]-1 for row in rows)
    assert report(binary,storage,*[f'{i:016x}:point {i}' for i in range(1,1025)])==[]
    print('PASS: 1024-point linear history stays compact; fully filtered history is empty',flush=True)


# Malformed graphs or TSV-breaking names must fail before emitting any selectable values.
def check_errors(binary,root):
    storage=fixture(root,[(1,0,'bad\nrow',False)],1);report(binary,storage,success=False)
    (root/'changes.dat.snapshots/0000000000000001.snap').write_bytes(snapshot(4*MiB,point=1,parent=0,name='okay'))
    before=tree_state(root)
    for extra in [['--json'],['--info'],['--hide','bad'],['--hide','0000000000000000:name']]:
        result=subprocess.run([binary,'--snapshots','--tsv','-f',str(storage),*extra],capture_output=True,text=True)
        assert result.returncode and not result.stdout and tree_state(root)==before
    for args in [['--tsv'],['--hide','0000000000000001:okay']]:
        result=subprocess.run([binary,'-f',str(storage),*args],capture_output=True,text=True)
        assert result.returncode and not result.stdout and tree_state(root)==before
    (root/'changes.dat.snapshots/0000000000000002.snap').write_bytes(snapshot(4*MiB,point=2,parent=99,name='orphan'))
    report(binary,storage,success=False)
    print('PASS: invalid combinations, unsafe fields and incomplete ancestry emit no rows or disk changes',flush=True)


if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('binary');args=parser.parse_args();binary=str(Path(args.binary).resolve())
    with tempfile.TemporaryDirectory(prefix='dynfilefs-menu-') as folder:
        root=Path(folder)
        for name,check in [('branches',check_branches),('linear',check_linear),('errors',check_errors)]:
            fixture_root=root/name;fixture_root.mkdir();check(binary,fixture_root)
