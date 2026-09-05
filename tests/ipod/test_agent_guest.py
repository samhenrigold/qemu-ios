#!/usr/bin/env python3
"""Opt-in native agent acceptance. Uses a fresh NAND overlay and owns its processes."""
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
parser.add_argument('--base-nand')
parser.add_argument('--baked', action='store_true')
args = parser.parse_args()
out = tempfile.mkdtemp(prefix='it-agent-guest-')
f = args.files
cfg = SimpleNamespace(out=out, files=f, base_nand=args.base_nand or f+'/nand-ultimate',
    nor=f+'/ios3/nor_7E18.bin', overlay=out+'/overlay', qemu=args.qemu,
    usbmuxd=args.usbmuxd, usbmuxd_ok=True, usb_port=r.free_port(1520,1539),
    mux_port=r.free_port(27400,27419), qmp_port=r.free_port(28200,28219),
    wifi=True, cpu=None, mem='128M', kernel_console=True,
    install_timeout=420, proxy_lo=28460, proxy_hi=28479)
os.environ['PATH'] = str(ROOT.parent/'qemu-ios-deps12/bin') + ':' + os.environ['PATH']
p = r.Procs()
d = r.Device(cfg, p, 'device')
r.START = time.time()
print('OUTPUT', out, flush=True)
try:
    d.start()
    ok, detail, lit = d.wait_for_home(240); assert ok, detail
    result = r.Result('launch')
    port = r.prepare_launcher(cfg, p, d, result); assert port, result.detail
    if not args.baked:
        result = r.guest_ssh(cfg, port, [], scp_from=str(ROOT/'contrib/it-agent/it_agent'), scp_to='/tmp/it_agent')
        assert result.returncode == 0, result
        result = r.guest_ssh(cfg, port, ['launchctl unload /System/Library/LaunchDaemons/com.qemu.it-pbd.plist; chmod 755 /tmp/it_agent; /tmp/it_agent </dev/null >/tmp/it_agent.log 2>&1 &'])
        assert result.returncode == 0, result
    deadline = time.monotonic() + 80
    while not r.itqmp.agent_alive(d.qmp):
        if time.monotonic() > deadline:
            raise AssertionError(r.guest_ssh(cfg, port, ['cat /tmp/it_agent.log /var/log/it_agent.log']))
        time.sleep(1)
    def agent(op, args='', body=b''):
        status, data = r.itqmp.agent(d.qmp, op, args, body)
        print(op, status, repr(data[:100]), flush=True)
        return status, data
    assert agent('ping') == (0, b'it_agent v1\n')
    status, data = agent("exec", 'printf \'uid=%s\\n\' "$UID"'); assert status == 0 and b"uid=0" in data, (status, data)
    body = bytes(range(256)) * 40
    assert agent('exec', 'cat', body) == (0, body)
    assert agent('put', '/tmp/agent binary 600', body) == (0, b'')
    assert agent('get', '/tmp/agent binary') == (0, body)
    assert agent('exec', 'printf error >&2; exit 7') == (7, b'error')
    assert agent('settime', '1000000000')[0] == 0
    time.sleep(3)
    status, data = agent('exec', 'date +%s'); assert status == 0 and abs(int(data) - time.time()) < 5
    assert agent('exec', 'sleep 2; printf alive') == (0, b'alive')
    assert r.itqmp.agent_alive(d.qmp)
    assert agent('lockstatus')[0] == 0
    assert agent('launch', 'com.apple.Preferences')[0] == 0
    time.sleep(2)
    status, data = agent('frontmost'); assert status == 0 and b'com.apple.Preferences' in data
    d.qmp.cmd("qom-set", path="/machine", property="pasteboard", value="Agent clipboard acceptance")
    deadline = time.monotonic() + 10
    while True:
        pb = d.qmp.cmd("qom-get", path="/machine", property="pasteboard-status")
        if pb.startswith("delivered:") and "Agent clipboard acceptance" in pb: break
        assert time.monotonic() < deadline, pb
        time.sleep(0.25)
    try:
        r.itqmp.agent(d.qmp, "exec", "sleep 3; touch /tmp/agent-cancel-failed", timeout=0.5)
        raise AssertionError("slow command unexpectedly completed")
    except TimeoutError:
        pass
    deadline = time.monotonic() + 10
    while not r.itqmp.agent_alive(d.qmp):
        assert time.monotonic() < deadline
        time.sleep(0.25)
    time.sleep(4)
    assert agent("exec", "test ! -e /tmp/agent-cancel-failed") == (0, b"")
    if args.baked:
        result = r.guest_ssh(cfg, port, ["killall it_agent"]); assert result.returncode == 0
        time.sleep(11)
        deadline = time.monotonic() + 90
        while not r.itqmp.agent_alive(d.qmp):
            assert time.monotonic() < deadline, "launchd agent did not recover"
            time.sleep(1)
        assert agent("ping") == (0, b"it_agent v1\n")
    print('PASS: native agent ping, root exec, binary stdin/files, status, clock, liveness', flush=True)
finally:
    if d.qmp: d.qmp.close()
    p.stop_all()
