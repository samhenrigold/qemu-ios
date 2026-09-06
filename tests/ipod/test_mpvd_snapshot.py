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
 movie=out/'fixture.m4v'
 subprocess.run(['ffmpeg','-v','error','-f','lavfi','-i','testsrc2=size=64x48:rate=30',
                 '-frames:v','8','-c:v','mpeg4','-bf','0','-g','30','-q:v','3',
                 '-enc_time_base','1:30','-f','m4v',str(movie)],check=True)
 marker=b'\x00\x00\x01\xb6'
 packets=[marker+data for data in movie.read_bytes().split(marker)[1:]]
 assert len(packets)==8 and packets[0][4]>>6==0
 assert all(data[4]>>6==1 for data in packets[1:])
 baseline=[]
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
     deadline=time.monotonic()+20
     while q.cmd('query-status')['status']=='inmigrate':
      assert time.monotonic()<deadline
      time.sleep(.05)
     def cmd(text):
      f.write((text+'\n').encode());reply=f.readline().decode().strip();assert reply.startswith('OK'),reply;return reply
     def w(offset,value):cmd(f'writel {0x39600000+offset:#x} {value:#x}')
     def read(offset):return int(cmd(f'readl {0x39600000+offset:#x}').split()[1],0)
     def irq():return bool(int(cmd('readl 0x38e01008').split()[1],0)&(1<<13))
     def submit(data):
      # Keep qtest lines bounded; large writes can fill its socket buffer.
      for offset in range(0,len(data),256):
       block=data[offset:offset+256]
       cmd(f'write {0x08001000+offset:#x} {len(block)} 0x{block.hex()}')
      for offset,value in {0x6006c:(4<<16)|3,0x1009c:5,0x10010:data[4]>>6,
                           0x60018:0x08001008,0x6001c:0x08001000+len(data),
                           0x6003c:0x08200000,0x60044:0x08210000}.items():w(offset,value)
      w(0x1000c,0x0c)
      assert read(0x10000)==4 and read(0x50000)==2
      return bytes.fromhex(cmd('read 0x08200000 3072').split()[1][2:])+bytes.fromhex(cmd('read 0x08210000 1536').split()[1][2:])
     if not phase:
      w(0,5)
      assert read(0)==(0 if enabled else 5)
      w(0x1000c,0x0c) # Invalid dimensions: decode-enabled hardware reports failure.
      assert read(0)==(2 if enabled else 5) and irq()==enabled
      if enabled:
       for packet in packets[:2]:submit(packet)
      q.cmd('migrate',uri='file:'+str(out/'snapshot'));deadline=time.monotonic()+30
      while True:
       state=q.cmd('query-migrate');assert state['status']!='failed',state
       if state['status']=='completed':break
       assert time.monotonic()<deadline,state;time.sleep(.1)
      if enabled:baseline=[submit(packet) for packet in packets[2:]]
     else:
      assert read(0)==(2 if enabled else 5) and irq()==enabled
      if enabled:
       assert [submit(packet) for packet in packets[2:]]==baseline
       print('PASS: native MPVD restored P-picture DMA matches uninterrupted decoding',flush=True)
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
