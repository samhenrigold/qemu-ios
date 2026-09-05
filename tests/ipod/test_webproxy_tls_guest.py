#!/usr/bin/env python3
"""Opt-in native iOS 3 TLS acceptance; fresh overlay, loopback server, no internet.
Requires built itproxy/httpget/ittrust helpers, QEMU, usbmuxd and OpenSSL 3.
The temporary CA is trusted only inside this disposable guest, then removed.
"""
import argparse
import os
from pathlib import Path
import tempfile
import time
from types import SimpleNamespace
import regress as r

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--files', default=str(ROOT.parent/'qemu-ios-files'))
parser.add_argument('--qemu', default=str(ROOT/'build-native14/qemu-build/qemu-system-arm'))
parser.add_argument('--usbmuxd', default=str(ROOT/'build-native14/build/usbmuxd/src/usbmuxd'))
parser.add_argument('--openssl', default=str(ROOT.parent/'qemu-ios-deps12/bin/openssl'))
args = parser.parse_args()
out = tempfile.mkdtemp(prefix='it-tls-guest-')
f = args.files
cfg = SimpleNamespace(out=out, files=f, base_nand=f+'/nand-ultimate',
    nor=f+'/ios3/nor_7E18.bin', overlay=out+'/overlay', qemu=args.qemu,
    usbmuxd=args.usbmuxd, usbmuxd_ok=True, usb_port=r.free_port(1520,1539),
    mux_port=r.free_port(27400,27419), qmp_port=r.free_port(28200,28219),
    wifi=True, cpu=None, mem='128M', kernel_console=True,
    install_timeout=420, proxy_lo=28460, proxy_hi=28479)
os.environ['PATH'] = str(ROOT.parent/'qemu-ios-deps12/bin') + ':' + os.environ['PATH']
routing = Path(out)/'routing'
class Capture(r.Procs):
 def spawn(self, argv, logpath, env=None):
  if argv[0] == cfg.qemu:
   import shlex
   command = shlex.quote(str(ROOT/'contrib/it-webproxy/itwebproxy')) + ' ' + shlex.quote(str(routing))
   argv[argv.index('-netdev')+1] += ',guestfwd=tcp:10.0.2.100:3128-cmd:' + command.replace(',', ',,')
  return super().spawn(argv, logpath, env)
p = Capture()
d = r.Device(cfg, p, 'device')
r.START = time.time()
print('OUTPUT', out, flush=True)
try:
 d.start()
 ok,detail,lit=d.wait_for_home(240);assert ok,detail
 result=r.Result('launch');port=r.prepare_launcher(cfg,p,d,result);assert port,result.detail
 src=str(ROOT/'contrib/it-proxy/itproxy')
 result=r.guest_ssh(cfg,port,[],scp_from=src,scp_to='/tmp/itproxy');assert result.returncode==0,result
 from pathlib import Path
 import subprocess
 openssl=args.openssl
 tls=Path(out)/'tls';tls.mkdir(mode=0o700)
 def ssl(*args):
  subprocess.run([openssl,*args],cwd=tls,check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
 ssl('req','-x509','-sha1','-newkey','rsa:2048','-nodes','-keyout','ca.key','-out','ca.pem','-days','2','-subj','/CN=Light Touch TLS Acceptance CA','-addext','basicConstraints=critical,CA:TRUE,pathlen:0','-addext','keyUsage=critical,keyCertSign,cRLSign')
 ssl('req','-new','-newkey','rsa:2048','-nodes','-keyout','leaf.key','-out','leaf.csr','-subj','/CN=10.0.2.100')
 (tls/'leaf.ext').write_text('basicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature,keyEncipherment\nextendedKeyUsage=serverAuth\nsubjectAltName=IP:10.0.2.100\n')
 ssl('x509','-req','-sha1','-in','leaf.csr','-CA','ca.pem','-CAkey','ca.key','-CAcreateserial','-out','leaf.pem','-days','2','-extfile','leaf.ext')
 ssl('x509','-in','ca.pem','-outform','DER','-out','ca.der')
 tlsport=r.free_port(28600,28619)
 server=p.spawn([openssl,'s_server','-accept',f'127.0.0.1:{tlsport}','-cert',str(tls/'leaf.pem'),'-key',str(tls/'leaf.key'),'-tls1','-cipher','AES128-SHA:@SECLEVEL=0','-www'],str(tls/'server.log'))
 routing.write_text(f'upstream\n127.0.0.1\n{tlsport}\n')
 for src,dest in [(str(ROOT/'contrib/it-proxy/httpget'),'/tmp/it-http'),(str(ROOT/'contrib/it-proxy/ittrust'),'/tmp/ittrust'),(str(tls/'ca.der'),'/tmp/it-ca.der')]:
  result=r.guest_ssh(cfg,port,[],scp_from=src,scp_to=dest);assert result.returncode==0,result
 print('PROXY OFF',r.guest_ssh(cfg,port,['/tmp/itproxy off']),flush=True)
 time.sleep(3)
 result=r.guest_ssh(cfg,port,['/tmp/it-http https://10.0.2.100:3128/'])
 print('UNTRUSTED',result.returncode,result.stdout[:700],flush=True);assert result.returncode!=0,result
 result=r.guest_ssh(cfg,port,['/tmp/ittrust add /tmp/it-ca.der']);print('ADD CA',result.returncode,result.stdout,flush=True);assert result.returncode==0,result
 result=r.guest_ssh(cfg,port,['/tmp/ittrust add /tmp/it-ca.der']);assert result.returncode==0,result
 result=r.guest_ssh(cfg,port,['/tmp/it-http https://10.0.2.100:3128/'])
 print('TRUSTED',result.returncode,result.stdout[:1400],flush=True);assert result.returncode==0 and 'HTTP 200' in result.stdout,result
 result=r.guest_ssh(cfg,port,['/tmp/ittrust remove /tmp/it-ca.der']);print('REMOVE CA',result.returncode,result.stdout,flush=True);assert result.returncode==0,result
 result=r.guest_ssh(cfg,port,['/tmp/ittrust remove /tmp/it-ca.der']);assert result.returncode==0,result
 result=r.guest_ssh(cfg,port,['/tmp/it-http https://10.0.2.100:3128/'])
 print('REMOVED',result.returncode,result.stdout[:700],flush=True);assert result.returncode!=0,result
 print('PASS native TLS1.0 AES128-SHA with guest-local CA trust and revocation',flush=True)

finally:
 if d.qmp:d.qmp.close()
 p.stop_all()
