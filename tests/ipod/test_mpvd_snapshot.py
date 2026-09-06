#!/usr/bin/env python3
"""Native MPVD mode, completion, IRQ acknowledgement and restore checks."""
from pathlib import Path
import os,socket,subprocess,tempfile,time,sys
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'imgtools'))
from itqmp import QMP
files=ROOT.parent/'qemu-ios-files'
with tempfile.TemporaryDirectory(prefix='it-mpvd-snapshot-') as tmp:
 out=Path(tmp)
 for enabled in (False, True):
  for phase in range(2):
   qpath=str(out/f'qmp{int(enabled)}-{phase}');tpath=str(out/f'qtest{int(enabled)}-{phase}')
   argv=[str(ROOT/'build-native14/qemu-build/qemu-system-arm'),'-S','-M',f'iPod-Touch,bootrom={files}/bootrom_240_4,nand={files}/nand,nor={files}/nor_n72ap.bin,nandrw={out}/overlay,mpvd-decode={"on" if enabled else "off"}','-m','128M','-display','none','-serial','null','-monitor','none','-qmp',f'unix:{qpath},server=on,wait=off','-qtest',f'unix:{tpath},server=on,wait=off','-qtest-log',str(out/f'qtest{phase}.log')]
   if phase:argv+=['-incoming','file:'+str(out/'snapshot')]
   with (out/f'qemu{int(enabled)}-{phase}.log').open('w') as log:
    child=subprocess.Popen(argv,stdout=log,stderr=log,env={k:v for k,v in os.environ.items() if not k.startswith('IT_')});q=None;t=None;f=None
    try:
     deadline=time.monotonic()+20
     while not Path(tpath).exists():
      assert child.poll() is None and time.monotonic()<deadline,(out/f'qemu{int(enabled)}-{phase}.log').read_text()
      time.sleep(.05)
     q=QMP(qpath,timeout=10);t=socket.socket(socket.AF_UNIX);t.settimeout(10);t.connect(tpath);f=t.makefile('rwb',buffering=0)
     def cmd(text):
      f.write((text+'\n').encode());reply=f.readline().decode().strip();assert reply.startswith('OK'),reply;return reply
     def w(offset,value):cmd(f'writel {0x39600000+offset:#x} {value:#x}')
     def read(offset):return int(cmd(f'readl {0x39600000+offset:#x}').split()[1],0)
     def irq():return bool(int(cmd('readl 0x38e01008').split()[1],0)&(1<<13))
     if not phase:
      w(0,5)
      assert read(0)==(0 if enabled else 5)
      w(0x1000c,0x0c) # Invalid dimensions: decode-enabled hardware reports failure.
      assert read(0)==(2 if enabled else 5) and irq()==enabled
      q.cmd('migrate',uri='file:'+str(out/'snapshot'));deadline=time.monotonic()+30
      while True:
       state=q.cmd('query-migrate');assert state['status']!='failed',state
       if state['status']=='completed':break
       assert time.monotonic()<deadline,state;time.sleep(.1)
     else:
      assert read(0)==(2 if enabled else 5) and irq()==enabled
      w(0,2)
      assert read(0)==(0 if enabled else 2) and not irq()
      q.cmd('system_reset')
      assert read(0)==0 and not irq()
      w(0x1000c,0x0c)
      assert read(0)==(2 if enabled else 0) and irq()==enabled
     q.cmd('quit');assert child.wait(timeout=10)==0
     print('PASS: MPVD', enabled, 'saved' if not phase else 'restored mode, IRQ, acknowledgement and reset',flush=True)
    finally:
     if f:f.close()
     if t:t.close()
     if q:q.close()
     if child.poll() is None:child.kill();child.wait()
