#!/usr/bin/env python3
"""Regression acceptance uses current activation records and fails skipped checks."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
from unittest.mock import patch
spec=importlib.util.spec_from_file_location('ipad_regress',Path(__file__).with_name('regress.py'))
R=importlib.util.module_from_spec(spec);spec.loader.exec_module(R)
import ffmpeg_guard
with tempfile.TemporaryDirectory(prefix='ipad-regress-inputs-') as tmp:
    d=Path(tmp);lock=d/'device.lock.json'
    for inputs,want in (({'activation':{'patch':{'strategy':'development-activation-shortcut'}}},True),
                        ({'activation_hook':{'path':'legacy'}},True),({'activation':None},False),({},False)):
        lock.write_text(json.dumps({'inputs':inputs}))
        a=SimpleNamespace(nand=None,device=str(d))
        R.device_args(a);assert a.activated is want
    for strict,want in ((False,0),(True,1)):
        argv=['regress.py','--checks','appinstall','--out',tmp]+(['--require-inputs'] if strict else [])
        with patch.object(R.sys,'argv',argv),patch.object(R,'device_args'),patch.object(ffmpeg_guard,'check',return_value=None),contextlib.redirect_stdout(io.StringIO()):
            assert R.main()==want
print('PASS current/legacy activation records and strict skipped-check acceptance')

# Persistence must establish that the bytes reached AFC before judging NAND,
# and must observe a guest shutdown rather than wait and kill the emulator.
import shutil
for failure in ('put','before','shutdown','after',None):
    with tempfile.TemporaryDirectory(prefix='persist-contract-') as tmp:
        d=Path(tmp);(d/'persist').mkdir();(d/'persist2').mkdir()
        events=[];payload=[]
        def run(argv):
            op=argv[1];events.append(op)
            if op=='put': payload.append(Path(argv[2]).read_bytes())
            else: Path(argv[-1]).write_bytes(b'wrong' if (failure=='before' and 'before' in argv[-1]) or (failure=='after' and 'back' in argv[-1]) else payload[0])
            return SimpleNamespace(returncode=1 if failure=='put' and op=='put' else 0,stderr='diagnostic')
        def shutdown(): events.append('shutdown');return failure!='shutdown'
        b=SimpleNamespace(dir=str(d/'persist'),run=run,powerdown=shutdown,stop=lambda:events.append('stop1'))
        b2=SimpleNamespace(dir=str(d/'persist2'),run=run,stop=lambda:events.append('stop2'))
        def boot(cfg,tag,result,**kw): events.append(tag);return (b if tag=='persist' else b2),'booted'
        r=R.Result('persist')
        with patch.object(R,'booted',side_effect=boot),patch.object(R.ipod,'log'):
            R.check_persist(SimpleNamespace(out=tmp),r)
        assert r.ok is (failure is None),(failure,r.detail)
        assert ('persist2' in events) is (failure in ('after',None)),events
        if failure not in ('put','before'): assert events.index('shutdown') < events.index('stop1')
print('PASS persistence write/readback, guest shutdown and post-reboot corruption gates')

# A transport connection log cannot prove lockdown is ready for this UDID.
answers=iter([SimpleNamespace(returncode=1,stdout=''),SimpleNamespace(returncode=0,stdout='3.2.2\n')])
b=SimpleNamespace(udid='intended',cfg=SimpleNamespace(product_version='3.2.2'),
                  qemu=SimpleNamespace(poll=lambda:None),run=lambda argv,timeout:next(answers))
with patch.object(R.time,'sleep') as sleep:
    assert R.Boot.wait_mux(b,timeout=5)
    assert sleep.call_count==1
b=SimpleNamespace(udid='intended',env=lambda:{'USBMUXD_SOCKET_ADDRESS':'127.0.0.1:27400'})
with patch.object(R.subprocess,'run') as run:
    R.Boot.run(b,['afcclient','put','file','/marker'])
    assert run.call_args.args[0]==['afcclient','-u','intended','put','file','/marker']
    assert run.call_args.kwargs['env']['USBMUXD_SOCKET_ADDRESS']=='127.0.0.1:27400'
print('PASS live USB readiness and intended-device routing')

# A cable-attached iPad can remain in iBoot's charging loop after a real halt.
# Host quit is permitted only after the PMU's guest-origin confirmation.
for confirmations,want in (([False,True],True),([False],False)):
    calls=[];responses=iter(confirmations)
    def cmd(name,**args):
        calls.append(name)
        if name=='qom-get': return next(responses)
    q=SimpleNamespace(cmd=cmd,close=lambda:calls.append('close'))
    process=SimpleNamespace(wait=lambda timeout:0)
    clock=iter([0,0,61]) if want else iter([0,61])
    with patch.object(R.itqmp,'agent_alive',return_value=True),patch.object(R.itqmp,'agent',return_value=(0,b'')),patch.object(R.itqmp.time,'sleep'),patch.object(R.itqmp.time,'monotonic',side_effect=lambda:next(clock)):
        assert R.itqmp.guest_powerdown(q,process,'test',log=lambda _:None,charging_halt=True) is want
    assert ('quit' in calls) is want
    assert calls[-1]=='close'
print('PASS charging halt requires guest PMU evidence before host quit')

# A halt RPC timeout is not permission to repeat it. Only PMU evidence can
# resolve its unknown outcome after iBoot restarted into charging mode.
for confirmed,want in ((True,True),(False,False)):
    calls=[]
    def cmd(name,**kw): calls.append(name);return confirmed
    q=SimpleNamespace(cmd=cmd,close=lambda:None)
    with patch.object(R.itqmp,'agent_alive',return_value=True),patch.object(R.itqmp,'agent',side_effect=TimeoutError('outcome unknown')) as request,patch.object(R.itqmp.time,'monotonic',side_effect=[0,61]):
        assert R.itqmp.guest_powerdown(q,SimpleNamespace(wait=lambda timeout:0),'test',log=lambda _:None,charging_halt=True) is want
        assert request.call_count==1
    assert ('quit' in calls) is want
print('PASS unknown halt outcome is observed, never retried')

with tempfile.TemporaryDirectory(prefix='ocr-cache-') as tmp:
    binary=Path(tmp)/'new-cache'/'ocr';commands=[]
    def run(argv,**kw):
        commands.append(argv)
        if argv[0]=='swiftc': Path(argv[argv.index('-o')+1]).write_bytes(b'compiled');return SimpleNamespace(returncode=0,stderr='')
        return SimpleNamespace(stdout='1 2 3 4 Word\n')
    with patch.object(R,'OCR_BIN',str(binary)),patch.object(R.subprocess,'run',side_effect=run):
        assert R.ocr('picture.ppm')=={'Word':(3,765)}
        assert R.ocr('picture.ppm')=={'Word':(3,765)}
    assert sum(c[0]=='swiftc' for c in commands)==1
    assert binary.read_bytes()==b'compiled'
print('PASS OCR creates and atomically publishes its cache on a clean worktree')
