#!/usr/bin/env python3
"""Opt-in snapshot acceptance using an isolated overlay and owned processes."""
import argparse
import os
from pathlib import Path
import tempfile
import time
from types import SimpleNamespace
import regress as r

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--files', default=str(ROOT.parent / 'qemu-ios-files'))
parser.add_argument('--base-nand')
parser.add_argument('--network', action='store_true')
parser.add_argument('--usb', action='store_true')
parser.add_argument('--expect-gles-block', action='store_true')
parser.add_argument('--audio', action='store_true')
args = parser.parse_args()
os.environ['PATH'] = str(ROOT.parent/'qemu-ios-deps12/bin') + ':' + os.environ['PATH']
out = tempfile.mkdtemp(prefix='it-snapshot-guest-')
f = args.files
cfg = SimpleNamespace(out=out, files=f, base_nand=args.base_nand or f+'/nand-agent-v2',
    nor=f+'/ios3/nor_7E18.bin', overlay=out+'/overlay',
    qemu=str(ROOT/'build-native14/qemu-build/qemu-system-arm'),
    usbmuxd=str(ROOT/'build-native14/build/usbmuxd/src/usbmuxd'), usbmuxd_ok=True,
    usb_port=r.free_port(1520,1539), mux_port=r.free_port(27400,27419),
    qmp_port=r.free_port(28200,28219), wifi=True, cpu=None, mem='128M', kernel_console=True,
    install_timeout=420, proxy_lo=28460, proxy_hi=28479)

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
    status, data = r.itqmp.agent(q, 'exec', '/tmp/snapshot-httpget http://10.0.2.2:%d/' % server.server_port)
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
    assert r.itqmp.agent(d.qmp, 'ping') == (0, b'it_agent v1\n')
    assert r.itqmp.agent(d.qmp, 'exec', 'printf snapshot-survived > /tmp/snapshot-marker') == (0, b'')
    if args.network:
        binary = (ROOT/'contrib/it-proxy/httpget').read_bytes()
        assert r.itqmp.agent(d.qmp, 'put', '/tmp/snapshot-httpget 755', binary) == (0, b'')
        check_network(d.qmp)
    if args.usb:
        udid, detail = r.wait_for_device(cfg, timeout=120)
        assert udid, detail
    if args.audio:
        result = r.Result('audio launcher')
        port = r.prepare_launcher(cfg, p, d, result)
        assert port, result.detail
        installed = r.run(['ideviceinstaller', 'install', str(ROOT/'contrib/it-harness/build/Harness.ipa')], cfg, 120)
        assert installed.returncode == 0, installed
        assert r.itqmp.agent(d.qmp, 'launch', 'com.qemuios.harness')[0] == 0
        time.sleep(4)
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
    if args.expect_gles_block:
        try:
            d.qmp.cmd('migrate', uri='file:' + snapshot)
        except RuntimeError as error:
            assert 'Live OpenGL ES state cannot be saved' in str(error), error
        else:
            raise AssertionError('unsafe GL snapshot was accepted')
        d.qmp.cmd('cont')
        assert r.itqmp.agent(d.qmp, 'ping') == (0, b'it_agent v1\n')
        print('PASS: unsafe GL save refused; guest remains usable', flush=True)
        raise SystemExit(0)
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
    assert r.itqmp.agent(d.qmp, 'ping') == (0, b'it_agent v1\n')
    assert r.itqmp.agent(d.qmp, 'get', '/tmp/snapshot-marker') == (0, b'snapshot-survived')
    status, data = r.itqmp.agent(d.qmp, 'exec', 'date +%s')
    assert status == 0 and abs(int(data) - time.time()) < 5, (status, data)
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
