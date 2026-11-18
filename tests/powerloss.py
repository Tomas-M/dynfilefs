from pathlib import Path
import json,os,shutil,signal,subprocess,sys,tempfile,time

binary=str(Path(sys.argv[1]).resolve());base=Path(sys.argv[2]).resolve();results=[];conversion=len(sys.argv)>3 and sys.argv[3]=='conversion'
# Only uniquely owned loop fixtures may be overwritten or removed by this test.
with tempfile.TemporaryDirectory(prefix='powerloss-',dir=base) as temporary:
    root=Path(temporary);baseline=root/'baseline.img';image=root/'live.img';crash=root/'crash.img';live=root/'live';view=root/'view';live.mkdir();view.mkdir()
    with baseline.open('xb') as stream:stream.truncate(1024*1024*1024)
    subprocess.run(['mkfs.fat','-F32','-s16',str(baseline)],check=True,stdout=subprocess.DEVNULL)
    subprocess.run(['mount','-o','loop,noatime',str(baseline),str(live)],check=True)
    try:
        subprocess.run([binary,'legacy' if conversion else 'prepare',str(live/'changes.dat')],check=True)
        legacy_part=(live/'changes.dat.0').read_bytes() if conversion else None
    finally:subprocess.run(['umount',str(live)],check=True)
    for operation in (['convert'] if conversion else ['create','rename','delete','checkpoint','finish','policy-write','policy-clear','policy-mark']):
        cut=int(sys.argv[4]) if len(sys.argv)>4 else 0;maximum=cut
        while cut<=maximum:
            subprocess.run(['cp','--sparse=always',str(baseline),str(image)],check=True)
            subprocess.run(['mount','-o','loop,noatime',str(image),str(live)],check=True)
            process=None
            try:
                with (root/'worker.log').open('w+') as log:
                    process=subprocess.Popen([binary,'run',str(live/'changes.dat'),operation,str(cut)],stdout=log,stderr=subprocess.STDOUT)
                    deadline=time.monotonic()+20
                    while time.monotonic()<deadline:
                        if process.poll() is not None:raise AssertionError((operation,cut,(root/'worker.log').read_text()))
                        if '\nState:\tT' in Path('/proc/'+str(process.pid)+'/status').read_text():break
                        time.sleep(.002)
                    else:raise TimeoutError((operation,cut))
                    text=(root/'worker.log').read_text();acknowledged='ACK ' in text
                    if acknowledged:maximum=max(maximum,int(text.split('ACK ')[-1].split()[0]))
                    # Concurrent namespace writeback must not free referenced FAT clusters early.
                    for directory in [live,live/'changes.dat.snapshots']:
                        if directory.exists():
                            fd=os.open(directory,os.O_RDONLY|os.O_DIRECTORY);os.fsync(fd);os.close(fd)
                    subprocess.run(['cp','--sparse=always',str(image),str(crash)],check=True)
                    if conversion:
                        # Non-journaled FAT needs its own recovery before the container can resume writes.
                        repaired=subprocess.run(['fsck.fat','-a',str(crash)],capture_output=True,text=True)
                        assert repaired.returncode in [0,1],repaired.stdout+repaired.stderr
                    subprocess.run(['mount','-o','loop,noatime',str(crash),str(view)],check=True)
                    try:
                        checked=subprocess.run([binary,'verify',str(view/'changes.dat'),operation,str(int(acknowledged))],capture_output=True,text=True)
                        if checked.returncode:
                            subprocess.run(['cp','--sparse=always',str(crash),str(base/('failed-'+operation+'-'+str(cut)+'.img'))],check=True)
                            (base/('failed-'+operation+'-'+str(cut)+'.log')).write_text(text+'\n'+checked.stdout+'\n'+checked.stderr)
                        assert checked.returncode==0,(operation,cut,acknowledged,checked.stdout,checked.stderr)
                        if conversion:assert (view/'changes.dat.0').read_bytes()==legacy_part
                    finally:subprocess.run(['umount',str(view)],check=True)
                    results.append({'operation':operation,'cut':cut,'acknowledged':acknowledged,'verified':True,'outer_fat_recovery':conversion})
            finally:
                if process and process.poll() is None:process.kill();process.wait(timeout=10)
                subprocess.run(['umount',str(live)],check=True)
            if cut%25==0:print(json.dumps({'operation':operation,'cut':cut,'maximum':maximum,'verified':len(results)}),flush=True)
            cut+=1
        print('PASS '+operation+' '+str(maximum)+' interrupted backing-I/O boundaries',flush=True)
    (base/('conversion-powerloss-results.json' if conversion else 'powerloss-policy-results.json')).write_text(json.dumps(results,indent=2)+'\n')
