#!/usr/bin/env python3
import argparse
import contextlib
import json
import os
from pathlib import Path
import platform
import statistics
import subprocess
import tempfile
import time


# Keep each benchmark's backing files in its selected filesystem.
@contextlib.contextmanager
def mounted(binary, root, trace=None):
    mount=root/'mount'
    mount.mkdir(exist_ok=True)
    command=[binary, '-f', str(root/'storage'), '-m', str(mount), '-s', '512', '-p', '32']
    traced=None
    if trace:
        traced=subprocess.Popen(['strace', '-f', '-c', '-e', 'trace=pread64,pwrite64,preadv,pwritev,preadv2,pwritev2,fsync,msync,splice', '-o', str(trace), *command], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    else:
        subprocess.run(command, check=True)
    for _ in range(200):
        if (mount/'virtual.dat').exists():
            break
        time.sleep(0.02)
    assert (mount/'virtual.dat').exists()
    pid=next(int(p.name) for p in Path('/proc').iterdir() if p.name.isdigit() and process_args(p).startswith(b'@') and str(root/'storage').encode()+b'\0' in process_args(p))
    try:
        yield mount/'virtual.dat', pid
    finally:
        subprocess.run(['umount', str(mount)], check=True)
        if traced:
            traced.wait(timeout=15)


# Ignore unrelated processes that exit while /proc is being enumerated.
def process_args(path):
    try:
        return (path/'cmdline').read_bytes()
    except (FileNotFoundError, ProcessLookupError):
        return b''


# /proc's process counters include CPU time consumed by all FUSE worker threads.
def cpu_seconds(pid):
    values=Path('/proc/'+str(pid)+'/stat').read_text().split(') ', 1)[1].split()
    return (int(values[11])+int(values[12]))/os.sysconf('SC_CLK_TCK')


# Use direct frontend I/O; backing-file cache remains enabled as in normal dynfilefs.
def fio(path, rw, bs, seconds, jobs=1, sync=False, size='512M'):
    command=['fio', '--name='+rw, '--filename='+str(path), '--rw='+rw, '--bs='+bs, '--ioengine=psync', '--direct=1', '--size='+size, '--numjobs='+str(jobs), '--group_reporting=1', '--output-format=json', '--randrepeat=1', '--refill_buffers=1', '--end_fsync=1']
    if jobs>1:
        command += ['--size=64M', '--offset_increment=64M']
    if seconds:
        command += ['--time_based=1', '--runtime='+str(seconds)]
    if sync:
        command += ['--fsync=1']
    result=json.loads(subprocess.check_output(command))
    assert all(job['error']==0 for job in result['jobs']), result
    return result['jobs'][0]


# Measure one case repeatedly and retain samples rather than only a headline number.
def measure(path, pid, case, seconds, repeats):
    name, rw, bs, jobs, sync=case
    samples=[]
    for _ in range(repeats):
        before=cpu_seconds(pid)
        started=time.monotonic()
        job=fio(path, rw, bs, seconds, jobs, sync)
        elapsed=time.monotonic()-started
        direction=job['read'] if rw.endswith('read') else job['write']
        samples.append({'mib_s':direction['bw_bytes']/1024/1024, 'iops':direction['iops'], 'latency_us':direction['clat_ns']['mean']/1000, 'sync_us':job.get('sync', {}).get('lat_ns', {}).get('mean', 0)/1000, 'daemon_cpu_percent':100*(cpu_seconds(pid)-before)/elapsed})
    result={'case':name, 'samples':samples, 'median':{key:statistics.median(sample[key] for sample in samples) for key in samples[0]}}
    print(name+': '+json.dumps(result['median']), flush=True)
    return result


# Build an identical fully allocated fixture before every version's timed workloads.
def benchmark(binary, parent, seconds, repeats):
    cases=[('seq-read-128k', 'read', '128K', 1, False), ('seq-write-128k', 'write', '128K', 1, False), ('seq-read-1m', 'read', '1M', 1, False), ('seq-write-1m', 'write', '1M', 1, False), ('rand-read-4k-8', 'randread', '4K', 8, False), ('rand-write-4k-8', 'randwrite', '4K', 8, False), ('fsync-4k', 'randwrite', '4K', 1, True)]
    results=[]
    with tempfile.TemporaryDirectory(prefix='dynfilefs-bench-', dir=parent) as folder:
        root=Path(folder)
        with mounted(binary, root) as (path, pid):
            first=fio(path, 'write', '128K', 0)
            results.append({'case':'first-write-512m', 'median':{'mib_s':first['write']['bw_bytes']/1024/1024}})
            for case in cases:
                results.append(measure(path, pid, case, seconds, repeats))
    return results


# Count storage syscalls separately so tracing cannot distort the timed results.
def trace_io(binary, parent, output):
    with tempfile.TemporaryDirectory(prefix='dynfilefs-trace-', dir=parent) as folder:
        root=Path(folder)
        with mounted(binary, root, output) as (path, pid):
            fio(path, 'write', '128K', 0, size='16M')
            fio(path, 'read', '128K', 0, size='16M')


if __name__=='__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('binary')
    parser.add_argument('output')
    parser.add_argument('--directory', required=True)
    parser.add_argument('--seconds', type=int, default=3)
    parser.add_argument('--repeats', type=int, default=3)
    parser.add_argument('--trace', action='store_true')
    args=parser.parse_args()
    binary=str(Path(args.binary).resolve())
    parent=Path(args.directory)
    parent.mkdir(parents=True, exist_ok=True)
    output=Path(args.output)
    data={'binary':binary, 'kernel':platform.release(), 'cpus':os.cpu_count(), 'fio':subprocess.check_output(['fio', '--version'], text=True).strip(), 'seconds':args.seconds, 'repeats':args.repeats, 'filesystem':subprocess.check_output(['findmnt', '-n', '-o', 'FSTYPE', '-T', str(parent)], text=True).strip(), 'results':benchmark(binary, parent, args.seconds, args.repeats)}
    output.write_text(json.dumps(data, indent=2)+'\n')
    if args.trace:
        trace_io(binary, parent, output.with_suffix('.strace'))
