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
args = parser.parse_args()
os.environ['PATH'] = str(ROOT.parent/'qemu-ios-deps12/bin') + ':' + os.environ['PATH']
out = tempfile.mkdtemp(prefix='it-snapshot-guest-')
f = args.files
cfg = SimpleNamespace(out=out, files=f, base_nand=args.base_nand or f+'/nand-agent-v2',
    nor=f+'/ios3/nor_7E18.bin', overlay=out+'/overlay',
    qemu=str(ROOT/'build-native14/qemu-build/qemu-system-arm'),
    usbmuxd=str(ROOT/'build-native14/build/usbmuxd/src/usbmuxd'), usbmuxd_ok=True,
    usb_port=r.free_port(1520,1539), mux_port=r.free_port(27400,27419),
    qmp_port=r.free_port(28200,28219), wifi=True, cpu=None, mem='128M', kernel_console=True)

class SnapshotProcs(r.Procs):
    incoming = None
    def spawn(self, argv, *args, **kwargs):
        if argv[0] == cfg.qemu and self.incoming:
            argv = argv + ['-incoming', 'file:' + self.incoming, '-S']
        return super().spawn(argv, *args, **kwargs)

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
    assert d.qmp.cmd('query-status')['running']
    r.to_png(d.qmp.shot(out+'/restored.ppm'), out+'/restored.png')
    print('PASS: home snapshot restore, guest agent rekey, file state and host clock', flush=True)
finally:
    if d and d.qmp: d.qmp.close()
    p.stop_all()
    if server:
        server.shutdown()
        server.server_close()
