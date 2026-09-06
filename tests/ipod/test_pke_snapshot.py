#!/usr/bin/env python3
"""Native migration preserves PKE operand SRAM, registers and loaded modulus."""
from pathlib import Path
import os,socket,subprocess,tempfile,time,sys
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'imgtools'))
from itqmp import QMP
files=ROOT.parent/'qemu-ios-files'
with tempfile.TemporaryDirectory(prefix='it-pke-snapshot-') as tmp:
 out=Path(tmp)
 for phase in range(2):
  qpath=str(out/f'qmp{phase}');tpath=str(out/f'qtest{phase}')
  argv=[str(ROOT/'build-native14/qemu-build/qemu-system-arm'),'-S','-M',f'iPod-Touch,bootrom={files}/bootrom_240_4,nand={files}/nand,nor={files}/nor_n72ap.bin,nandrw={out}/overlay','-m','128M','-display','none','-serial','null','-monitor','none','-qmp',f'unix:{qpath},server=on,wait=off','-qtest',f'unix:{tpath},server=on,wait=off','-qtest-log',str(out/f'qtest{phase}.log')]
  if phase:argv+=['-incoming','file:'+str(out/'snapshot')]
  with (out/f'qemu{phase}.log').open('w') as log:
   child=subprocess.Popen(argv,stdout=log,stderr=log,env={k:v for k,v in os.environ.items() if not k.startswith('IT_')});q=None;t=None;f=None
   try:
    deadline=time.monotonic()+20
    while not Path(tpath).exists():
     assert child.poll() is None and time.monotonic()<deadline,(out/f'qemu{phase}.log').read_text()
     time.sleep(.05)
    q=QMP(qpath,timeout=10);t=socket.socket(socket.AF_UNIX);t.settimeout(10);t.connect(tpath);f=t.makefile('rwb',buffering=0)
    def cmd(text):
     f.write((text+'\n').encode());reply=f.readline().decode().strip();assert reply.startswith('OK'),reply;return reply
    def w(offset,value):cmd(f'writel {0x3d000000+offset:#x} {value:#x}')
    def read(offset):return int(cmd(f'readl {0x3d000000+offset:#x}').split()[1],0)
    if not phase:
     w(0,0x78);w(0x14,129);w(0x800,19)
     w(0x800+28*64,5);w(0x800+29*64,7);w(0x800+27*64,0xbeef)
     w(0xc,28<<24|29<<16|27);w(0x10,1<<28);w(8,8)
     w(0x800,23) # The loaded modulus must survive independently of SRAM.
     q.cmd('migrate',uri='file:'+str(out/'snapshot'));deadline=time.monotonic()+30
     while True:
      state=q.cmd('query-migrate');assert state['status']!='failed',state
      if state['status']=='completed':break
      assert time.monotonic()<deadline,state;time.sleep(.1)
    else:
     assert read(0)==0x78 and read(0x14)==129 and read(0x800+27*64)==0xbeef
     w(8,1)
     assert read(0x800+27*64)==(-5*7*pow(2,-528,19))%19
     assert read(0x10)==1<<28 and read(8)==0
     for word in range(1,16):assert read(0x800+27*64+4*word)==0
    q.cmd('quit');assert child.wait(timeout=10)==0
    print('PASS: PKE', 'state saved' if not phase else 'loaded modulus, signs, selectors and full SRAM restored',flush=True)
   finally:
    if f:f.close()
    if t:t.close()
    if q:q.close()
    if child.poll() is None:child.kill();child.wait()
