#!/usr/bin/env python3
"""Opt-in native iOS 3 TLS acceptance without SSH or a shell; fresh overlay, loopback server, no internet.

The guest's native NSURLConnection (contrib/it-proxy/httpget, staged and run by the agent) fetches
https://10.0.2.100:3128/ through the host itwebproxy guestfwd (the PAC's private-IP rule sends that
address DIRECT, so no guest proxy setting is involved). The temporary CA is trusted the iPad way: a
configuration profile offered over lockdown's stock com.apple.mobile.MCInstall by the app's own
lockdown-mcinstall tool, then Install tapped on the device. Untrusted before, trusted after.
Requires built httpget, contrib/it-webproxy/itwebproxy, QEMU, usbmuxd, OpenSSL 3 and libimobiledevice.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile
import time
from types import SimpleNamespace
import regress as r

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--files', default=str(ROOT.parent/'qemu-ios-files'))
parser.add_argument('--base-nand', help='default: <files>/nand-current (the shipping image)')
parser.add_argument('--device', help='a device.py device directory (nand/, nor.bin, iBoot.bin, gid-blobs.bin)')
parser.add_argument('--qemu', default=str(ROOT/'build/qemu-system-arm'))
parser.add_argument('--usbmuxd', default=os.path.expanduser('~/Developer/usbmuxd-qemu/usbmuxd/src/usbmuxd'))
parser.add_argument('--openssl', default=str(ROOT.parent/'qemu-ios-deps12/bin/openssl'))
parser.add_argument('--mcinstall-source', default=os.path.expanduser('~/Developer/LightTouchMac-multidevice/scripts/lockdown-mcinstall.c'),
                    help="the app's MCInstall client (scripts/lockdown-mcinstall.c), built here")
args = parser.parse_args()
out = tempfile.mkdtemp(prefix='it-tls-guest-')
f = args.files
cfg = SimpleNamespace(out=out, files=f, overlay=out+'/overlay', qemu=args.qemu,
    usbmuxd=args.usbmuxd, usbmuxd_ok=True, usb_port=r.free_port(1520,1539),
    mux_port=r.free_port(27400,27419), qmp_port=r.free_port(28200,28219),
    wifi=True, cpu=None, mem='128M', kernel_console=False, install_timeout=420)
if args.device:
    cfg.device = args.device
    cfg.base_nand, cfg.nor = args.device+'/nand', args.device+'/nor.bin'
    cfg.direct_iboot, cfg.gid_blobs = args.device+'/iBoot.bin', args.device+'/gid-blobs.bin'
else:
    cfg.base_nand = os.path.realpath(args.base_nand or f+'/nand-current')
    cfg.nor = f+'/ios3/nor_7E18.bin'
os.makedirs(cfg.overlay)
routing = Path(out)/'routing'
cfg.web_proxy_config = str(routing)
routing.write_text('off\n')
tls = Path(out)/'tls'; tls.mkdir(mode=0o700)
mcinstall = tls/'lockdown-mcinstall'
subprocess.run(['cc', '-O2', '-o', str(mcinstall), args.mcinstall_source, '-I/opt/homebrew/include',
                '-L/opt/homebrew/lib', '-limobiledevice-1.0', '-lplist-2.0'], check=True)
def ssl(*argv):
    subprocess.run([args.openssl, *argv], cwd=tls, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
ssl('req','-x509','-sha1','-newkey','rsa:2048','-nodes','-keyout','ca.key','-out','ca.pem','-days','2','-subj','/CN=Light Touch TLS Acceptance CA','-addext','basicConstraints=critical,CA:TRUE,pathlen:0','-addext','keyUsage=critical,keyCertSign,cRLSign')
ssl('req','-new','-newkey','rsa:2048','-nodes','-keyout','leaf.key','-out','leaf.csr','-subj','/CN=10.0.2.100')
(tls/'leaf.ext').write_text('basicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature,keyEncipherment\nextendedKeyUsage=serverAuth\nsubjectAltName=IP:10.0.2.100\n')
ssl('x509','-req','-sha1','-in','leaf.csr','-CA','ca.pem','-CAkey','ca.key','-CAcreateserial','-out','leaf.pem','-days','2','-extfile','leaf.ext')
ssl('x509','-in','ca.pem','-outform','DER','-out','ca.der')
# 342 KB in default 16 KiB records: multi-segment TCP and multi-page crypto
# buffers (the A4 AES straddle bug broke exactly this on the iPad).
big = b''.join(b'line %06d of the big TLS body, padding padding padding\n' % i for i in range(6000))
(tls/'big.txt').write_bytes(big)

p = r.Procs()
d = r.Device(cfg, p, 'device')
r.START = time.time()
print('OUTPUT', out, flush=True)
def shot(name):
    r.to_png(d.qmp.shot(out+'/'+name+'.ppm'), out+'/'+name+'.png')
try:
    tlsport = r.free_port(28600,28619)
    os.chdir(tls)                                   # s_server -WWW serves its cwd
    p.spawn([args.openssl,'s_server','-accept',f'127.0.0.1:{tlsport}','-cert',str(tls/'leaf.pem'),'-key',str(tls/'leaf.key'),
             '-tls1','-cipher','AES128-SHA:@SECLEVEL=0','-WWW'], str(tls/'server.log'))
    d.start()
    ok, detail, lit = d.wait_for_home(600); assert ok, detail
    ok, detail = r.ensure_agent(d.qmp); assert ok, detail
    control = r.AgentControl(d.qmp)
    udid, detail = r.wait_for_device(cfg); assert udid, detail
    routing.write_text(f'upstream\n127.0.0.1\n{tlsport}\n')
    assert r.itqmp.agent(d.qmp, 'put', '/tmp/it-http 755', (ROOT/'contrib/it-proxy/httpget').read_bytes())[0] == 0
    def fetch(url, timeout=90):
        result = r.spawn(control, ['/tmp/it-http', url], timeout=timeout)
        print('FETCH', url, result.returncode, result.stdout[:200].split('\n')[0], flush=True)
        return result
    result = fetch('https://10.0.2.100:3128/')
    assert result.returncode != 0, result
    ok, detail = r.unlock(cfg, control, d); assert ok, detail
    result = subprocess.run([str(mcinstall), str(tls/'ca.der')], env=r.mux_env(cfg), capture_output=True, text=True, timeout=60)
    print('MCINSTALL', result.returncode, result.stdout, result.stderr, flush=True)
    assert result.returncode == 0 and 'Acknowledged' in result.stdout, result
    time.sleep(6)
    shot('profile-0')
    # 3.1.3 Preferences (ManagedConfiguration): Install Profile's Install button, the unsigned-profile
    # alert's Install Now, then Done on Profile Installed. The screenshots are kept as evidence.
    for step, (x, y, wait) in enumerate(((262, 160, 4), (92, 326, 10), (288, 42, 3)), 1):
        d.qmp.tap(x, y)
        time.sleep(wait)
        shot('profile-%d' % step)
    d.qmp.home()
    time.sleep(2)
    result = fetch('https://10.0.2.100:3128/')
    assert result.returncode == 0 and 'HTTP 200' in result.stdout, result
    result = fetch('https://10.0.2.100:3128/big.txt', timeout=120)
    body = result.stdout.split('\n', 1)[1].encode() if '\n' in result.stdout else b''
    print('BIG', result.returncode, len(body), flush=True)
    assert result.returncode == 0 and body == big, (result.returncode, len(body))
    print('PASS native TLS1.0 AES128-SHA, a 342 KB body, CA trusted by an MCInstall profile (no shell, no ittrust)', flush=True)
    assert d.powerdown(), 'guest shutdown not confirmed'
finally:
    if d.qmp: d.qmp.close()
    p.stop_all()
