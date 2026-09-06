#!/usr/bin/env python3
"""Native NOR flash and in-flight page-buffer migration on paused machines."""
from pathlib import Path
import socket,subprocess,tempfile,time,sys,shutil,hashlib,argparse
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'imgtools'))
from itqmp import QMP
root=ROOT;files=root.parent/'qemu-ios-files'
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--persistent',action='store_true')
args=parser.parse_args()
base=files/'nor_n72ap.bin';base_hash=hashlib.sha256(base.read_bytes()).digest()
with tempfile.TemporaryDirectory(prefix='it-nor-snapshot-') as tmp:
 out=Path(tmp)
 def rejected(path, expected):
  machine=f'iPod-Touch,bootrom={files}/bootrom_240_4,nand={files}/nand,nor={base},nandrw={out}/rejected-overlay,nor-rw={path}'
  result=subprocess.run([str(root/'build-native14/qemu-build/qemu-system-arm'),'-S','-M',machine,'-m','128M','-display','none','-serial','null','-monitor','none'],stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=15)
  assert result.returncode!=0 and expected in result.stdout,result.stdout.decode(errors='replace')
 if args.persistent:
  shutil.copyfile(base,out/'nor.bin')
  rejected(base,b'private copy')
  (out/'hardlink').hardlink_to(base);rejected(out/'hardlink',b'private copy')
  (out/'small').write_bytes(b'bad');rejected(out/'small',b'exactly')
  rejected(out/'missing',b'must exist')
 for phase in range(3 if args.persistent else 2):
  qpath=str(out/f'qmp{phase}');tpath=str(out/f'qtest{phase}')
  argv=[str(root/'build-native14/qemu-build/qemu-system-arm'),'-S','-M',f'iPod-Touch,bootrom={files}/bootrom_240_4,nand={files}/nand,nor={files}/nor_n72ap.bin,nandrw={out}/overlay','-m','128M','-display','none','-serial','null','-monitor','none','-qmp',f'unix:{qpath},server=on,wait=off','-qtest',f'unix:{tpath},server=on,wait=off','-qtest-log',str(out/f'qtest{phase}.log')]
  if args.persistent:argv[argv.index('-M')+1]+=f',nor-rw={out}/nor.bin'
  if phase==1:argv+=['-incoming','file:'+str(out/'snapshot')]
  with (out/f'qemu{phase}.log').open('w') as log:
   child=subprocess.Popen(argv,stdout=log,stderr=log);q=None;t=None
   try:
    deadline=time.monotonic()+20
    while not Path(tpath).exists():
     assert child.poll() is None and time.monotonic()<deadline,(out/f'qemu{phase}.log').read_text()
     time.sleep(.05)
    q=QMP(qpath,timeout=10);t=socket.socket(socket.AF_UNIX);t.settimeout(10);t.connect(tpath);f=t.makefile('rwb',buffering=0)
    def cmd(s):
     f.write((s+'\n').encode());r=f.readline().decode().strip();assert r.startswith('OK'),r;return r
    def w(a,v):cmd(f'writel {a:#x} {v:#x}')
    def byte(v):
     w(0x3c300000,12);w(0x3c300010,v);w(0x3c300034,1);w(0x3c30004c,1);w(0x3c300000,1)
     return int(cmd('readl 0x3c300020').split()[1],0)&255
    def tx(data):
     w(0x3cf001e0,14);r=[byte(b) for b in data];w(0x3cf001e0,15);return r
    if not phase:
     if args.persistent:rejected(out/'nor.bin',b'lock')
     tx([6]);tx([1,0]);tx([6]);tx([0x20,0,0,0]);tx([6]);tx([2,0,0,0,0x12,0x34])
    if phase:
     w(0x3cf001e0,15)  # Commit the page buffered before migration.
     assert tx([3,0,0,0,255,255,255])[-3:]==[0x12,0x34,0x56]
    else:
     assert tx([3,0,0,0,255,255])[-2:]==[0x12,0x34]
     tx([6]);w(0x3cf001e0,14)
     for value in [2,0,0,2,0x56]:byte(value)
    if not phase:
     q.cmd('migrate',uri='file:'+str(out/'snapshot'));deadline=time.monotonic()+30
     while True:
      state=q.cmd('query-migrate');assert state['status']!='failed',(state,(out/f'qemu{phase}.log').read_text())
      if state['status']=='completed':break
      assert time.monotonic()<deadline,state;time.sleep(.1)
    q.cmd('quit');assert child.wait(timeout=10)==0
    print('PASS:','NOR program before migration' if not phase else ('NOR content after migration' if phase==1 else 'NOR content in fresh process'),flush=True)
   finally:
    if t:t.close()
    if q:q.close()
    if child.poll() is None:child.kill();child.wait()
assert hashlib.sha256(base.read_bytes()).digest()==base_hash, 'base NOR changed'
