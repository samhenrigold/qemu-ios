#!/usr/bin/env python3
"""Opt-in snapshot acceptance using an isolated overlay and owned processes."""
import argparse
import json
import os
from pathlib import Path
import tempfile
import time
from types import SimpleNamespace
import regress as r

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--files', default=str(Path.home()/'Developer/qemu-ios-files'))
parser.add_argument('--base-nand')
parser.add_argument('--device', help='matched prepared firmware, NOR, identity and helper set')
parser.add_argument('--qemu')
parser.add_argument('--usbmuxd')
parser.add_argument('--ipa', default=str(ROOT/'contrib/it-harness/build/Harness.ipa'))
parser.add_argument('--httpget', default=str(ROOT/'contrib/it-proxy/httpget'))
parser.add_argument('--out')
parser.add_argument('--network', action='store_true')
parser.add_argument('--usb', action='store_true')
parser.add_argument('--gles', action='store_true',
                    help='save and restore with the Harness GL scene running (live GL state)')
parser.add_argument('--audio', action='store_true')
args = parser.parse_args()
if args.gles and args.audio:
    parser.error('--gles and --audio need separate runs so each fixture remains foreground')
out = args.out or tempfile.mkdtemp(prefix='it-snapshot-guest-')
Path(out).mkdir(parents=True, exist_ok=True)
f = args.files
cfg = SimpleNamespace(out=out, files_dir=f, device=args.device, base_nand=args.base_nand,
    nor=None, direct_iboot=None, gid_blobs=None, bootrom=None, guest_package=None,
    overlay=out+'/overlay', ipa=args.ipa,
    qemu=args.qemu or str(next((q for q in (ROOT/'build-reuse/qemu-system-arm',
        ROOT/'build-native14/qemu-build/qemu-system-arm', ROOT/'build/qemu-system-arm') if q.exists()),
        ROOT/'build/qemu-system-arm')),
    usbmuxd=args.usbmuxd or str(Path.home()/'Developer/usbmuxd-qemu/usbmuxd/src/usbmuxd'),
    usbmuxd_ok=True, usb_port=r.free_port(1520,1539), mux_port=r.free_port(27400,27419),
    qmp_port=r.free_port(28200,28219), wifi=args.network, cpu=None, mem='128M', kernel_console=True,
    install_timeout=420, proxy_lo=28460, proxy_hi=28479)
r.configure_device(cfg)
assert cfg.board == 'n72ap', 'this snapshot fixture targets N72'

class SnapshotProcs(r.Procs):
    incoming = None
    def spawn(self, argv, *rest, **kwargs):
        if argv[0] == cfg.qemu and args.audio:
            argv = list(argv)
            argv[argv.index('-audio') + 1] = 'driver=wav,path=' + out + ('/after.wav' if self.incoming else '/before.wav')
        if argv[0] == cfg.qemu and self.incoming:
            argv = argv + ['-incoming', 'file:' + self.incoming, '-S']
        return super().spawn(argv, *rest, **kwargs)

server = None
if args.network:
    from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
    import threading
    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            body = b'snapshot-network-ok'
            self.send_response(200)
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        def log_message(self, *args): pass
    server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()

def check_network(q):
    status, data = r.itqmp.agent(q, 'spawn', body=b'/tmp/snapshot-httpget\0' + ('http://10.0.2.2:%d/' % server.server_port).encode() + b'\0')
    assert status == 0 and b'HTTP 200' in data and b'snapshot-network-ok' in data, (status, data)

p = SnapshotProcs()
d = None
r.START = time.time()
print('OUTPUT', out, flush=True)
try:
    d = r.Device(cfg, p, 'before')
    d.start()
    ok, detail, _ = d.wait_for_home(240)
    assert ok, detail
    deadline = time.monotonic() + 90
    while not r.itqmp.agent_alive(d.qmp):
        assert time.monotonic() < deadline, 'agent did not start'
        time.sleep(1)
    status, hello = r.itqmp.agent(d.qmp, 'ping')
    assert status == 0 and hello.startswith(b'it_agent v'), (status, hello)
    assert r.itqmp.agent(d.qmp, 'put', '/tmp/snapshot-marker 644', b'snapshot-survived') == (0, b'')
    assert r.itqmp.agent(d.qmp, 'sync') == (0, b'')
    if args.network:
        binary = Path(args.httpget).read_bytes()
        assert r.itqmp.agent(d.qmp, 'put', '/tmp/snapshot-httpget 755', binary) == (0, b'')
        check_network(d.qmp)
    if args.usb:
        udid, detail = r.wait_for_device(cfg, timeout=120)
        assert udid, detail
    if args.gles:
        # The Harness GL scene: a cyan/magenta fixture with a rotating
        # triangle, so a live context has textures, matrices and state to lose.
        result = r.Result('gles launcher')
        port = r.prepare_app_control(cfg, p, d, result)
        assert port, result.detail
        unlocked, detail = r.unlock(cfg, port, d)
        assert unlocked, detail
        installed = r.run(['ideviceinstaller', 'install', args.ipa], cfg, 120)
        assert installed.returncode == 0, installed
        assert r.itqmp.agent(d.qmp, 'launch', 'com.qemuios.harness')[0] == 0
        time.sleep(4)
        d.qmp.tap(150, 79)
        time.sleep(r.GLES_SETTLE_S)
        gl_before = r.quad_signature(d.qmp.shot(out+'/gles-before.ppm'))[:2]
        assert min(gl_before) >= r.GLES_QUAD_MIN, ('GL scene not up before save', gl_before)
    if args.audio:
        result = r.Result('audio launcher')
        port = r.prepare_app_control(cfg, p, d, result)
        assert port, result.detail
        unlocked, detail = r.unlock(cfg, port, d)
        assert unlocked, detail
        installed = r.run(['ideviceinstaller', 'install', args.ipa], cfg, 120)
        assert installed.returncode == 0, installed
        assert r.itqmp.agent(d.qmp, 'launch', 'com.qemuios.harness')[0] == 0
        time.sleep(4)
        for _ in range(16):
            r.itqmp.button(d.qmp, 'volup', hold_ms=100)
        time.sleep(2)
        # Move the seventh row into the menu viewport. Hold before releasing
        # so momentum does not move the target after the controlled drag.
        r.itqmp.move(d.qmp, 160, 290)
        d.qmp.cmd('input-send-event', events=[{'type':'btn','data':{'down':True,'button':'left'}}])
        for step in range(1, 27):
            r.itqmp.move(d.qmp, 160, 290 - step * 5)
            time.sleep(.05)
        time.sleep(.5)
        d.qmp.cmd('input-send-event', events=[{'type':'btn','data':{'down':False,'button':'left'}}])
        time.sleep(.5)
        r.to_png(d.qmp.shot(out+'/audio-menu.ppm'), out+'/audio-menu.png')
        d.qmp.tap(160, 211)
        time.sleep(.25)
        status, tree = r.itqmp.agent(d.qmp, 'uidump')
        Path(out+'/audio-ui.txt').write_bytes(tree)
        assert status == 0 and b'RUNNING audio stereo.wav' in tree, (status, tree[-1500:])
    print('GLES contexts before save:', d.qmp.cmd('qom-get', path='/machine', property='gles-contexts'), flush=True)
    d.qmp.cmd('stop')
    snapshot = out + '/state'
    d.qmp.cmd('migrate', uri='file:' + snapshot)
    deadline = time.monotonic() + 90
    while True:
        migration = d.qmp.cmd('query-migrate')
        if migration['status'] == 'completed': break
        assert migration['status'] not in ('failed', 'cancelled'), migration
        assert time.monotonic() < deadline, migration
        time.sleep(.25)
    d.qmp.close()
    d.qmp = None
    p.stop_all()
    time.sleep(2)
    p = SnapshotProcs()
    p.incoming = snapshot
    d = r.Device(cfg, p, 'after')
    d.start()
    d.qmp.cmd('cont')
    deadline = time.monotonic() + 30
    while not r.itqmp.agent_alive(d.qmp):
        assert time.monotonic() < deadline, 'restored agent did not rekey'
        time.sleep(.25)
    status, hello = r.itqmp.agent(d.qmp, 'ping')
    assert status == 0 and hello.startswith(b'it_agent v'), (status, hello)
    assert r.itqmp.agent(d.qmp, 'get', '/tmp/snapshot-marker') == (0, b'snapshot-survived')
    udid, detail = r.wait_for_device(cfg, timeout=120)
    assert udid, detail
    clock = r.run(['ideviceinfo', '-k', 'TimeIntervalSince1970'], cfg, 30)
    assert clock.returncode == 0 and abs(float(clock.stdout.strip()) - time.time()) < 5, clock
    if args.network:
        check_network(d.qmp)
        print('PASS: HTTP 200 through guest Wi-Fi before and after restore', flush=True)
    if args.usb:
        udid, detail = r.wait_for_device(cfg, timeout=120)
        assert udid, detail
        print('PASS: USB re-enumeration and lockdown pairing after restore', flush=True)
    if args.audio:
        assert b'com.qemuios.harness' in r.itqmp.agent(d.qmp, 'frontmost')[1]
        time.sleep(6)
    if args.gles:
        gles = d.qmp.cmd('qom-get', path='/machine', property='gles-contexts')
        assert gles > 0, 'no GL context after restore'
        time.sleep(2)
        a_ppm, b_ppm = d.qmp.shot(out+'/gles-after-1.ppm'), None
        time.sleep(1)
        b_ppm = d.qmp.shot(out+'/gles-after-2.ppm')
        gl_after = r.quad_signature(a_ppm)[:2]
        assert all(abs(x - y) < 0.02 for x, y in zip(gl_before, gl_after)), (gl_before, gl_after)
        # The triangle rotates every frame: two dumps a second apart differ
        # only if the restored context keeps presenting.
        assert open(a_ppm, 'rb').read() != open(b_ppm, 'rb').read(), 'GL frame frozen after restore'
        print('PASS: GL scene restored (magenta/cyan %.3f/%.3f, was %.3f/%.3f) and still presenting'
              % (gl_after + gl_before), flush=True)
    assert d.qmp.cmd('query-status')['running']
    r.to_png(d.qmp.shot(out+'/restored.ppm'), out+'/restored.png')
    print('PASS: home snapshot restore, guest agent rekey, file state and host clock', flush=True)
finally:
    if d and d.qmp: d.qmp.close()
    p.stop_all()
    if server:
        server.shutdown()
        server.server_close()

if args.audio:
    import wave
    import numpy as np
    with wave.open(out+'/after.wav', 'rb') as audio:
        assert audio.getnchannels() == 2 and audio.getsampwidth() == 2
        rate = audio.getframerate()
        samples = np.frombuffer(audio.readframes(audio.getnframes()), dtype='<i2').reshape(-1, 2)
    active = np.max(np.abs(samples.astype(np.int32)), axis=1) > 100
    assert np.count_nonzero(active) >= 2 * rate, 'less than two seconds of restored audio'
    for channel, expected in enumerate((440, 880)):
        spectrum = np.abs(np.fft.rfft(samples[:, channel]))
        peak = np.argmax(spectrum[1:]) + 1
        hz = peak * rate / len(samples)
        assert abs(hz - expected) < 2, (channel, hz)
    print('PASS: restored stereo playback has left 440 Hz and right 880 Hz for at least two seconds', flush=True)
