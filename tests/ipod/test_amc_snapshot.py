#!/usr/bin/env python3
"""Native AMC startup modes, reset, restore and incompatible-mode rejection."""
from pathlib import Path
import os,socket,subprocess,tempfile,time,sys,struct,hashlib
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'imgtools'))
from itqmp import QMP
files=ROOT.parent/'qemu-ios-files'
with tempfile.TemporaryDirectory(prefix='it-amc-snapshot-') as tmp:
 out=Path(tmp)
 for mode in ("registers", "handshake", "decode"):
  baseline = None
  for phase in range(3):
   qpath=str(out/f'qmp{mode}-{phase}');tpath=str(out/f'qtest{mode}-{phase}')
   argv=[str(ROOT/'build-native14/qemu-build/qemu-system-arm'),'-S','-device','loader,addr=0x08002000,data=0xeafffffe,data-len=4','-device','loader,addr=0x08002000,cpu-num=0','-M',f'iPod-Touch,bootrom={files}/bootrom_240_4,nand={files}/nand,nor={files}/nor_n72ap.bin,nandrw={out}/overlay,audio-hw=on,amc-mode={mode if phase != 2 else ("handshake" if mode == "registers" else "registers")}','-m','128M','-display','none','-serial','null','-monitor','none','-qmp',f'unix:{qpath},server=on,wait=off','-qtest',f'unix:{tpath},server=on,wait=off','-qtest-log',str(out/f'qtest{phase}.log')]
   if phase:argv+=['-incoming','file:'+str(out/'snapshot')]
   with (out/f'qemu{mode}-{phase}.log').open('w') as log:
    child=subprocess.Popen(argv,stdout=log,stderr=log,env={k:v for k,v in os.environ.items() if not k.startswith('IT_')});q=None;t=None;f=None
    try:
     if phase == 2:
      assert child.wait(timeout=15)!=0
      assert 'ipodtouch.amc' in (out/f'qemu{mode}-{phase}.log').read_text()
      print('PASS: AMC',mode,'rejects mismatched restore',flush=True)
      continue
     deadline=time.monotonic()+20
     while not Path(tpath).exists():
      assert child.poll() is None and time.monotonic()<deadline,(out/f'qemu{mode}-{phase}.log').read_text()
      time.sleep(.05)
     q=QMP(qpath,timeout=10);t=socket.socket(socket.AF_UNIX);t.settimeout(10);t.connect(tpath);f=t.makefile('rwb',buffering=0)
     deadline=time.monotonic()+20
     while q.cmd('query-status')['status']=='inmigrate':
      assert time.monotonic()<deadline
      time.sleep(.05)
     def cmd(text):
      f.write((text+'\n').encode());reply=f.readline().decode().strip();assert reply.startswith('OK'),reply;return reply
     def w(offset,value):cmd(f'writel {0x38500000+offset:#x} {value:#x}')
     def read(offset):return int(cmd(f'readl {0x38500000+offset:#x}').split()[1],0)
     def memory_write(address, data):
      cmd(f'write {address:#x} {len(data)} 0x{data.hex()}')
     def memory_read(address, size):
      return bytes.fromhex(cmd(f'read {address:#x} {size}').split()[1][2:])
     def advance_audio():
      # Production builds have only TCG, not qtest's synthetic clock. Run a
      # RAM branch-to-self briefly; no firmware or guest drivers execute.
      q.cmd('cont');time.sleep(.003);q.cmd('stop')
     def finish_audio():
      digest=hashlib.sha256(); frames=[]
      for tick in range(100):
       status=read(0xa9c)
       if status&4:
        header=memory_read(0x22028000,0x12)
        slots=[i for i in range(2) if struct.unpack_from('<H',header,0xa+i*4)[0]]
        assert len(slots)==1,header.hex()
        slot=slots[0]; size=struct.unpack_from('<H',header,0xc+slot*4)[0]*2
        assert size==4096
        frames.append(slot);digest.update(memory_read(0x22028100+slot*4096,size))
        memory_write(0x2202800a+slot*4,b'\0\0');w(0xc48,4)
       if status&0x40000:
        assert len(frames)==64 and frames==[i%2 for i in range(64)]
        return digest.hexdigest(),frames
       advance_audio()
      raise AssertionError('Restored AMC input never completed')
     if not phase:
      w(0x10,4)
      assert read(0x14)==(0 if mode=='registers' else 7)
      w(0x204,0xbeef)
      assert read(0x204)==0xbeef
      if mode=='decode':
       for offset,value in {0x940:0x84006e00,0x960:0xc600b800,0x964:0x848cba5d,0x968:0xc013f7fb}.items():w(offset,value)
       memory_write(0x2202ff00,struct.pack('<6H',7,0,0,4,0,0))
       data=bytes.fromhex('20680001a0000e')*64
       memory_write(0x08001000,data)
       memory_write(0x08000000,struct.pack('<3I',0,len(data)<<16,0x08001000))
       w(0xa8c,0x40004);w(0x100,0x08000001)
       advance_audio()
       assert read(0x100)==0 and read(0xa9c)==4
      q.cmd('migrate',uri='file:'+str(out/'snapshot'));deadline=time.monotonic()+30
      while True:
       state=q.cmd('query-migrate');assert state['status']!='failed',state
       if state['status']=='completed':break
       assert time.monotonic()<deadline,state;time.sleep(.1)
      if mode=='decode':baseline=finish_audio()
     else:
      assert read(0x204)==0xbeef, (mode,q.cmd("query-status"),hex(read(0x204)))
      assert read(0x14)==(0 if mode=='registers' else 7)
      if mode=='decode':
       assert finish_audio()==baseline
       print('PASS: AMC real VMState restores queued PCM, unread codec frames, alternating buffers and final DMA completion',flush=True)
      q.cmd('system_reset')
      assert read(0x204)==0 and read(0x14)==0
      w(0x10,4)
      assert read(0x14)==(0 if mode=='registers' else 7)
     q.cmd('quit');assert child.wait(timeout=10)==0
     print('PASS: AMC',mode,'saved' if not phase else 'restored registers and reset mode',flush=True)
    finally:
     if f:f.close()
     if t:t.close()
     if q:q.close()
     if child.poll() is None:child.kill();child.wait()
