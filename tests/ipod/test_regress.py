#!/usr/bin/env python3
"""Device-free checks for regression verdicts and IPA identity handling."""
import os
import json
import socket
from pathlib import Path
import plistlib
import subprocess
import struct
import tempfile
from types import SimpleNamespace
from unittest.mock import patch
import zipfile

import regress as R

BUNDLE = "org.example.Test"

with tempfile.TemporaryDirectory(prefix="regress-test-") as work:
    work = Path(work)
    ipa = work / "app.ipa"

    def archive(entries):
        with zipfile.ZipFile(ipa, "w") as z:
            for name, value in entries:
                z.writestr(name, value)

    root = "Payload/Test.app/Info.plist"
    info = {"CFBundleIdentifier": BUNDLE}
    for fmt in (plistlib.FMT_XML, plistlib.FMT_BINARY):
        archive([(root, plistlib.dumps(info, fmt=fmt)),
                 ("Payload/Test.app/PlugIns/Other.app/Info.plist", b"ignored")])
        assert R.ipa_bundle_id(ipa) == BUNDLE
    for entries in ([], [(root, plistlib.dumps({}))],
                    [(root, plistlib.dumps(info)),
                     ("Payload/Other.app/Info.plist", plistlib.dumps(info))]):
        archive(entries)
        try:
            R.ipa_bundle_id(ipa)
            raise AssertionError("invalid IPA accepted")
        except ValueError:
            pass
    archive([(root, plistlib.dumps(info))])

    cfg = SimpleNamespace(ipa=str(ipa), install_timeout=0, mux_port=1)
    for data, rc, expected in (
            ([info], 0, True),
            ([{"CFBundleIdentifier": BUNDLE + ".Other"}], 0, False),
            ([info], 1, False),
            ({"CFBundleIdentifier": BUNDLE}, 0, False)):
        listing = SimpleNamespace(stdout=plistlib.dumps(data).decode(),
                                  stderr=BUNDLE, returncode=rc)
        with patch.object(R, "run", return_value=listing):
            assert R.app_is_installed(cfg, BUNDLE) is expected
    for text in (BUNDLE, "<?xml version='1.0'?><plist><broken>"):
        with patch.object(R, "run", return_value=SimpleNamespace(
                stdout=text, stderr="", returncode=0)):
            assert not R.app_is_installed(cfg, BUNDLE)

    # A listed app cannot excuse a failed/timed-out install; no implicit XFAIL.
    for code, listed, expected in ((0, True, True), (1, True, False),
                                    (0, False, False), (None, True, False)):
        proc = SimpleNamespace(returncode=code, poll=lambda: code)
        procs = SimpleNamespace(spawn=lambda *a, **kw: proc, stop=lambda p: None)
        result = R.Result("appinstall")
        with patch.object(R, "app_is_installed", return_value=listed), patch.object(R, "log"):
            assert bool(R.check_appinstall(cfg, procs, SimpleNamespace(dir=str(work)),
                                          result)) is expected
        assert not result.xfail

    # Exercise the shell's actual reset verdict with a fake controller trace.
    for name, script in {
        "qemu": "#!/bin/sh\necho '[BT] cmd 0xfc4e'\nexec /bin/sleep 30\n",
        "sleep": "#!/bin/sh\n/bin/sleep 0.1\n",
        "python3": "#!/bin/sh\nexit 1\n",
    }.items():
        path = work / name
        path.write_text(script)
        path.chmod(0o755)
    env = dict(os.environ, QEMU=str(work / "qemu"),
               PATH=str(work) + os.pathsep + os.environ["PATH"])
    result = subprocess.run(["bash", str(Path(__file__).with_name(
        "bluetooth-bringup-check.sh")), "unused-image", "2"], env=env,
        text=True, capture_output=True, timeout=5)
    assert result.returncode == 1 and "QMP reset failed" in result.stderr, result
    assert "PASS:" not in result.stdout

# A sparse volume must retain the alternate header beyond the old 128000 cap.
with tempfile.TemporaryDirectory(prefix="fsck-test-") as work:
    from ftlmap import predict

    work = Path(work)
    base, overlay = work / "base", work / "overlay"
    base.mkdir()
    overlay.mkdir()
    header = bytearray(4096)
    header[1024:1026] = b"H+"
    struct.pack_into(">II", header, 1064, 4096, 128001)
    assert R.hfs_volume_blocks(header) == 128001
    for size, blocks in ((512, 128001), (4096, 0), (4096, 2097153)):
        bad = bytearray(header)
        struct.pack_into(">II", bad, 1064, size, blocks)
        try:
            R.hfs_volume_blocks(bad)
            raise AssertionError("invalid geometry accepted")
        except ValueError:
            pass
    try:
        R.hfs_volume_blocks(bytes(4096))
        raise AssertionError("missing HFS+ signature accepted")
    except ValueError:
        pass

    def page(directory, block, data):
        cs, number = predict(block)
        path = directory / ("cs%d" % cs) / ("%d.page" % number)
        path.parent.mkdir(exist_ok=True)
        path.write_bytes(data)
        return path

    page(base, 0, header)
    page(base, 128000, b"z" * 4096)
    page(base, 70000, b"b" * 4096)
    changed = page(overlay, 70000, b"o" * 4096)
    image = work / "volume.img"
    assert R.compose_fsck_volume(base, overlay, image) == (128001, 1)
    assert image.stat().st_size == 128001 * 4096
    with image.open("rb") as volume:
        volume.seek(69999 * 4096)
        assert volume.read(4096) == bytes(4096)
        assert volume.read(4096) == b"o" * 4096
        volume.seek(128000 * 4096)
        assert volume.read(4096) == b"z" * 4096
    changed.write_bytes(b"short")
    try:
        R.compose_fsck_volume(base, overlay, image)
        raise AssertionError("truncated overlay page accepted")
    except ValueError:
        pass

    cfg = SimpleNamespace(base_nand=str(base), overlay=str(overlay), out=str(work))
    for code in (0, 8):
        def command(argv, **kwargs):
            if argv[0] == "fsck_hfs":
                return SimpleNamespace(returncode=code, stderr="",
                    stdout="Invalid volume free block count\nVolume bitmap needs repair for under-allocation\n")
            return SimpleNamespace(returncode=0, stdout="/dev/disk42\n", stderr="")
        result = R.Result("fsck")
        with patch.object(R, "compose_fsck_volume", return_value=(128001, 1)), \
                patch.object(R.subprocess, "run", side_effect=command), patch.object(R, "log"):
            assert R.check_fsck(cfg, True, result) is (code == 0)
        assert (work / "fsck.log").read_text().startswith("Invalid volume")

print("Regression harness identity, failure verdicts, Bluetooth reset, and full-volume fsck checks passed")

# Exercise the shared transport with real streams, including events consumed
# while waiting for an unrelated command reply.
def qmp_stream(messages, eof=True):
    q = R.itqmp.QMP.__new__(R.itqmp.QMP)
    q.s, peer = socket.socketpair()
    q.f = q.s.makefile("rwb")
    q.shutdown_event = None
    q.reset_count = 0
    peer.sendall(b"".join(json.dumps(m).encode() + b"\n" for m in messages))
    if eof:
        peer.shutdown(socket.SHUT_WR)
    return q, peer


guest = {"event": "SHUTDOWN", "data": {"guest": True, "reason": "guest-shutdown"}}
host = {"event": "SHUTDOWN", "data": {"guest": False, "reason": "host-signal"}}
q, peer = qmp_stream([{"event": "RESET"}, {"event": "RESET"},
                      {"return": {"status": "running"}}])
assert q.cmd("query-status") == {"status": "running"}
assert q.reset_count == 2
q.close()
peer.close()
q, peer = qmp_stream([guest, {"return": {"status": "shutdown"}}])
assert q.cmd("query-status") == {"status": "shutdown"}
assert q.wait_for_guest_shutdown(0.01)
q.close()
assert q.f.closed and q.s.fileno() == -1
peer.close()

for messages, eof, error in (([host], True, RuntimeError), ([], True, EOFError),
                             ([{"event": "SHUTDOWN", "data": {"guest": True, "reason": "guest-reset"}}], True, RuntimeError),
                             ([], False, TimeoutError)):
    q, peer = qmp_stream(messages, eof)
    try:
        q.wait_for_guest_shutdown(0.01)
        raise AssertionError("unconfirmed shutdown accepted")
    except error:
        pass
    finally:
        q.close()
        peer.close()

# A host signal that exits QEMU successfully is still an unclean shutdown.
for event, exit_code, expected in ((host, 0, False), (guest, 0, True),
                                   (guest, 1, False), (None, 0, False)):
    q, peer = qmp_stream([event] if event else [])
    dev = SimpleNamespace(cfg=SimpleNamespace(), procs=None, tag='test', qmp=q,
                          qemu=SimpleNamespace(wait=lambda timeout: exit_code))
    with patch.object(R, 'ensure_guest_ssh', return_value=(1234, None)), \
         patch.object(R, 'guest_ssh', return_value=SimpleNamespace(returncode=0, stdout='', stderr='')), \
         patch.object(R.os.path, 'exists', return_value=True), patch.object(R, 'log'):
        assert R.Device.powerdown(dev) is expected
    assert dev.qmp is None and q.f.closed and q.s.fileno() == -1
    peer.close()
print("QMP checks passed: retained guest shutdown, host signal rejection, EOF, timeout, and closed streams")

with tempfile.TemporaryDirectory(prefix='qmp-frame-') as directory:
    path = Path(directory)/'frame.ppm'
    class FrameQMP:
        def cmd(self, name, **args):
            assert name == 'screendump'
            Path(args['filename']).write_bytes(b'P6\n1 1\n255\n\x00\x7f\xff')
    assert R.itqmp.shot_ppm(FrameQMP(), path, 14, timeout=0) == str(path)
    try:
        R.itqmp.shot_ppm(FrameQMP(), path, 100, timeout=0)
        raise AssertionError('truncated framebuffer accepted')
    except TimeoutError:
        pass
print('QMP raw framebuffer completeness checks passed')

# Agent failures must fail the check, including a byte-corrupted successful get.
assert set(R.DEFAULT_CHECKS) <= set(R.ALL_CHECKS)
assert R.APP_IPA_DEFAULT == R.HARNESS_IPA
for failure in (None, 'ping', 'exec', 'put', 'get'):
    saved = {}
    calls = []
    def agent(q, op, args='', body=b''):
        calls.append((op, args))
        if op == 'exec' and args.startswith('rm -f '):
            return 0, b''
        if op == failure:
            return (0, b'corrupt') if op == 'get' else (5, b'failed')
        if op == 'put':
            saved['body'] = body
        return 0, {'ping': b'it_agent v1\n', 'exec': b'42\n',
                   'put': b'', 'get': saved.get('body', b'') }[op]
    result = R.Result('agent')
    with patch.object(R.itqmp, 'agent_alive', return_value=True), \
         patch.object(R.itqmp, 'agent', side_effect=agent), patch.object(R, 'log'):
        assert R.check_agent(None, None, SimpleNamespace(qmp=object()), result) is (failure is None)
    assert calls[-1][0] == 'exec' and calls[-1][1].startswith('rm -f /tmp/regress-agent-')
print('Agent regression detects command failures and binary corruption')
