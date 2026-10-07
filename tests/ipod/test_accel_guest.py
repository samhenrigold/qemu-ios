#!/usr/bin/env python3
"""Native Harness accelerometer callbacks, axis signs and guest polling trace."""
import os, re, tempfile, time
from pathlib import Path
from types import SimpleNamespace
import regress as r
root=Path(__file__).resolve().parents[2]
f=str(root.parent/'qemu-ios-files')
os.environ['PATH']=str(root.parent/'qemu-ios-deps12/bin')+':'+os.environ['PATH']
os.environ['IT_ACCEL_TRACE']='1'
out=tempfile.mkdtemp(prefix='it-accel-guest-')
cfg=SimpleNamespace(out=out,files=f,base_nand=f+'/nand-agent-v2',nor=f+'/ios3/nor_7E18.bin',
 overlay=out+'/overlay',qemu=str(root/'build-native14/qemu-build/qemu-system-arm'),
 usbmuxd=str(root/'build-native14/build/usbmuxd/src/usbmuxd'),usbmuxd_ok=True,
 usb_port=r.free_port(1520,1539),mux_port=r.free_port(27400,27419),qmp_port=r.free_port(28200,28219),
 wifi=False,cpu=None,mem='128M',kernel_console=True,install_timeout=420,proxy_lo=28460,proxy_hi=28479)
r.START=time.time();p=r.Procs();d=r.Device(cfg,p,'device')
print('OUTPUT',out,flush=True)
try:
 d.start();ok,detail,_=d.wait_for_home(240);assert ok,detail
 deadline=time.monotonic()+60
 while not r.itqmp.agent_alive(d.qmp):
  assert time.monotonic()<deadline,'agent unavailable';time.sleep(1)
 udid,detail=r.wait_for_device(cfg,timeout=120);assert udid,detail
 installed=r.run(['ideviceinstaller','install',str(root/'contrib/it-harness/build/Harness.ipa')],cfg,120)
 assert installed.returncode==0,installed
 result=r.Result('launcher');port=r.prepare_launcher(cfg,p,d,result);assert port,result.detail
 ok,detail=r.unlock(cfg,port,d);assert ok,detail
 assert r.itqmp.agent(d.qmp,'launch','com.qemuios.harness')[0]==0
 time.sleep(4)
 for _ in range(3):
  r.itqmp.move(d.qmp,160,290)
  d.qmp.cmd('input-send-event',events=[{'type':'btn','data':{'down':True,'button':'left'}}])
  for step in range(1,27):
   r.itqmp.move(d.qmp,160,290-step*5);time.sleep(.05)
  time.sleep(.5)
  d.qmp.cmd('input-send-event',events=[{'type':'btn','data':{'down':False,'button':'left'}}])
  time.sleep(.5)
 r.to_png(d.qmp.shot(out+'/menu.ppm'),out+'/menu.png')
 d.qmp.tap(160,241);time.sleep(2)
 def attitude(pitch,roll):
  for name,value in [('pose','flat'),('pitch',pitch),('roll',roll)]:
   d.qmp.cmd('qom-set',path='/machine',property='accel-'+name,value=value)
  time.sleep(.7)
  status,tree=r.itqmp.agent(d.qmp,'uidump');assert status==0
  Path(out+'/tilt-%s-%s.txt'%(pitch,roll)).write_bytes(tree)
  m=re.search(rb'Tilt #(\d+)\s+x ([-\d.]+)\s+y ([-\d.]+)\s+z ([-\d.]+)',tree)
  assert m,tree[-2500:]
  return tuple(float(x) for x in m.groups()[1:])
 # 7E18 UIKit preserves raw X/Y signs and inverts raw Z (18 mg/count).
 rest=attitude(0,0);pitch=attitude(20,0);roll=attitude(0,20)
 print('CALLBACKS rest=%s pitch=%s roll=%s'%(rest,pitch,roll),flush=True)
 assert abs(rest[0])<.08 and abs(rest[1])<.08 and rest[2]>.8,rest
 assert pitch[1]>.2 and pitch[2]>.7,pitch
 assert roll[0]<-.2 and roll[2]>.7,roll
 trace=Path(d.dir+'/qemu.log').read_text(errors='replace')
 assert '[IT_ACCEL] OUT_X polls=' in trace
 print('PASS: Harness sees sampled flat rest, mounted pitch/roll and accelerometer polling trace',flush=True)
 assert d.powerdown(), 'guest shutdown not confirmed'
finally:
 if d.qmp:d.qmp.close()
 p.stop_all()
