#!/usr/bin/env python3
"""Native QOM attitude controls, including properties supplied before realize."""
import tempfile
import time
from pathlib import Path
from types import SimpleNamespace
import regress as r
root=Path(__file__).resolve().parents[2]
files=root.parent/'qemu-ios-files'
out=tempfile.mkdtemp(prefix='it-attitude-qmp-')
cfg=SimpleNamespace(out=out, files=str(files),base_nand=str(files/'nand-agent-v2'),
    nor=str(files/'ios3/nor_7E18.bin'),overlay=out+'/overlay',
    qemu=str(root/'build-native14/qemu-build/qemu-system-arm'),usbmuxd_ok=False,
    usb_port=r.free_port(1520,1539),qmp_port=r.free_port(28200,28219),wifi=False,cpu=None,mem='128M')
class Procs(r.Procs):
    def spawn(self,argv,*rest,**kwargs):
        if argv[0]==cfg.qemu:
            argv=list(argv)+['-S']
            index=argv.index('-M')+1
            argv[index]+=',accel-pose=flat,accel-pitch=30,accel-roll=30'
        return super().spawn(argv,*rest,**kwargs)
r.START=time.time()
p=Procs();d=r.Device(cfg,p,'device')
try:
    d.start();q=d.qmp
    def get(name):return q.cmd('qom-get',path='/machine',property='accel-'+name)
    def put(name,value):return q.cmd('qom-set',path='/machine',property='accel-'+name,value=value)
    def vector():return tuple(get(axis) for axis in 'xyz')
    assert get('pose')=='flat' and get('pitch')==30 and get('roll')==30
    assert vector()==(-28,32,-48),vector()
    assert get('rate-hz')==0
    put('rate-hz',200);assert get('rate-hz')==200
    for invalid in (-1,401,1<<40):
        try:put('rate-hz',invalid)
        except RuntimeError:pass
        else:raise AssertionError('invalid sample rate accepted')
    assert get('rate-hz')==200
    put('roll',0);assert vector()==(0,32,-55)
    put('pitch',90);assert vector()==(0,64,0)
    put('pose','upright');assert vector()==(0,0,-64)
    put('pitch',0);put('roll',90);assert vector()==(-64,0,0)
    for name,value in [('pose','sideways'),('pitch',181),('roll',-181)]:
        try:put(name,value)
        except RuntimeError:pass
        else:raise AssertionError('invalid attitude accepted')
        assert vector()==(-64,0,0)
    put('x',1<<40);assert get('x')==127
    put('x',-(1<<40));assert get('x')==-128
    put('orientation',5);assert vector()==(0,0,-64) and get('pose')=='flat'
    put('orientation',3);assert vector()==(-64,0,0) and get('roll')==90
    print('PASS: initial and live attitude QOM properties, mounted axes, raw overrides and invalid inputs')
finally:
    if d.qmp:d.qmp.close()
    p.stop_all()
