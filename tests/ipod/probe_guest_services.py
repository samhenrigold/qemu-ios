#!/usr/bin/env python3
"""Feasibility probe for docs/archive/guest-services-plan.md. Uses no SSH.

One headless boot of a 3.1.3 image on a throwaway overlay (the base is never
written), then:
  1. which stock lockdown services answer, one libimobiledevice tool at a time;
  2. whether concurrent lockdown sessions still fail;
  3. the prototype shell-free agent RPCs (`spawn`, `sync`) replacing
     `killall SpringBoard` and `sync`, end to end.

    timeout 570 python3 tests/ipod/probe_guest_services.py \
        --qemu ../qemu-ios-ipad1/build/qemu-system-arm
"""
import argparse
import concurrent.futures as cf
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parent))
import regress as r  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
DDI = Path.home() / ("Developer/qemu-ios-files/ipad1/sdk/x-DeveloperDiskImageReleased/Payload/"
                     "Platforms/iPhoneOS.platform/DeviceSupport/3.1.3/DeveloperDiskImage.dmg")
ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
ap.add_argument('--files', default=str(ROOT.parent / 'qemu-ios-files'))
ap.add_argument('--qemu', default=str(ROOT.parent / 'qemu-ios-ipad1/build/qemu-system-arm'))
ap.add_argument('--usbmuxd', default=str(ROOT.parent / 'qemu-ios/build-native14/build/usbmuxd/src/usbmuxd'))
ap.add_argument('--base-nand', default=None)
ap.add_argument('--agent', default=str(ROOT / 'contrib/it-agent/it_agent'), help='prototype agent to stage')
ap.add_argument('--keep', action='store_true', help='keep the output directory (overlay is always deleted)')
args = ap.parse_args()

signal.signal(signal.SIGTERM, lambda *_: sys.exit(143))  # so `timeout` still runs the cleanup
out = tempfile.mkdtemp(prefix='it-guest-probe-')
f = args.files
cfg = SimpleNamespace(out=out, files=f, base_nand=args.base_nand or f + '/nand-current.new',
    nor=f + '/ios3/nor_7E18.bin', overlay=out + '/overlay', qemu=args.qemu,
    usbmuxd=args.usbmuxd, usbmuxd_ok=True, usb_port=r.free_port(1540, 1559),
    mux_port=r.free_port(27420, 27439), qmp_port=r.free_port(28220, 28239),
    wifi=False, cpu=None, mem='128M', kernel_console=True, direct_iboot=f + '/ios3/iBoot.bin')
os.environ['PATH'] = '/opt/homebrew/bin:' + os.environ['PATH']
procs = r.Procs()
dev = r.Device(cfg, procs, 'device')
r.START = time.time()
print('OUTPUT', out, flush=True)


def t():
    return '%5.0fs' % (time.time() - r.START)


def tool(argv, timeout=40, stdin=None):
    p = r.run(argv, cfg, timeout, stdin=stdin)
    text = (p.stdout + p.stderr).strip().replace('\n', ' | ')
    return p.returncode, text


def report(name, rc, text, width=180):
    print('%s %-34s rc=%-3s %s' % (t(), name, rc, text[:width]), flush=True)


def agent(op, argstr='', body=b'', timeout=65):
    status, data = r.itqmp.agent(dev.qmp, op, argstr, body, timeout=timeout)
    print('%s agent %-10s %-40s -> %d %r' % (t(), op, argstr[:40], status, data[:120]), flush=True)
    return status, data


def argv_body(*argv):
    return b''.join(a.encode() + b'\0' for a in argv)


try:
    dev.start()
    ok, detail, _ = dev.wait_for_home(300)
    print(t(), 'home screen:', ok, detail, flush=True)
    assert ok, detail
    udid, detail = r.wait_for_device(cfg, 240)
    print(t(), 'paired:', udid, detail, flush=True)
    assert udid

    # --- 1. stock services, strictly one session at a time -----------------
    print('\n== services, sequential', flush=True)
    rc, text = tool(['ideviceinfo', '-k', 'ProductVersion'])
    report('lockdown (ideviceinfo)', rc, text)
    rc, text = tool(['ideviceinfo', '-q', 'com.apple.disk_usage'])
    report('lockdown disk_usage domain', rc, text)
    rc, text = tool(['afcclient'], stdin='ls /\nquit\n')
    report('com.apple.afc', rc, text)
    rc, text = tool(['ideviceinstaller', 'list', '--user'], 60)
    report('installation_proxy', rc, text)
    ids = [line.split(',')[0] for line in text.split(' | ')[1:] if ',' in line]
    if ids:
        rc, text = tool(['afcclient', '--documents', ids[0]], stdin='ls /\nquit\n')
        report('house_arrest (%s)' % ids[0], rc, text)
    syslog = procs.spawn(['idevicesyslog'], out + '/syslog-seq.log', env=r.mux_env(cfg))
    time.sleep(8)
    procs.stop(syslog)
    lines = Path(out, 'syslog-seq.log').read_text(errors='replace').splitlines()
    report('syslog_relay (8 s)', 0 if len(lines) > 1 else 1, '%d lines; last: %s' % (len(lines), lines[-1] if lines else ''))
    crash = Path(out, 'crash')
    crash.mkdir()
    rc, text = tool(['idevicecrashreport', '-k', '-e', str(crash)], 60)
    report('crashreportmover/copymobile', rc, text)
    rc, text = tool(['idevicenotificationproxy', 'post', 'com.apple.itunes-client.syncCancelRequest'])
    report('notification_proxy', rc, text)
    rc, text = tool(['idevicediagnostics', 'diagnostics', 'All'])
    report('diagnostics_relay', rc, text)
    rc, text = tool(['idevicescreenshot', out + '/shot-before-ddi.tiff'])
    report('screenshotr (no DDI)', rc, text)
    rc, text = tool(['ideviceimagemounter', 'list'])
    report('mobile_image_mounter list', rc, text, 600)
    if DDI.exists():
        rc, text = tool(['ideviceimagemounter', '-t', 'Developer', str(DDI), str(DDI) + '.signature'], 120)
        report('mobile_image_mounter mount DDI', rc, text, 900)
        rc, text = tool(['idevicescreenshot', out + '/shot-ddi.tiff'], 60)
        report('screenshotr (3.1.3 DDI)', rc, text)

    # --- 2. concurrent lockdown sessions -------------------------------------
    print('\n== services, concurrent', flush=True)
    syslog = procs.spawn(['idevicesyslog'], out + '/syslog-conc.log', env=r.mux_env(cfg))
    time.sleep(3)
    jobs = [(['ideviceinfo', '-k', 'DeviceName'], None),
            (['afcclient'], 'ls /\nquit\n'),
            (['ideviceinstaller', 'list', '--user'], None),
            (['afcclient'], 'ls /\nquit\n'),
            (['ideviceinfo', '-q', 'com.apple.disk_usage'], None),
            (['afcclient'], 'ls /\nquit\n')]
    for rnd in range(2):
        with cf.ThreadPoolExecutor(len(jobs)) as pool:
            results = list(pool.map(lambda j: tool(j[0], 60, j[1]), jobs))
        for (argv, _), (rc, text) in zip(jobs, results):
            report('round %d %s' % (rnd, ' '.join(argv[:2])), rc, text, 140)
    # Install-shaped load: parallel AFC uploads next to instproxy/lockdown queries.
    blob = Path(out, 'blob.bin')
    blob.write_bytes(os.urandom(8 << 20))
    upload = [(['afcclient'], 'put %s /Downloads/probe-%d.bin\nquit\n' % (blob, i)) for i in range(3)]
    upload += [(['ideviceinstaller', 'list', '--user'], None), (['ideviceinfo', '-k', 'DeviceName'], None)] * 2
    t0 = time.time()
    with cf.ThreadPoolExecutor(len(upload)) as pool:
        results = list(pool.map(lambda j: tool(j[0], 180, j[1]), upload))
    for (argv, _), (rc, text) in zip(upload, results):
        report('load %s' % ' '.join(argv[:2]), rc, text[-140:], 140)
    report('load wall time', 0, '%.1fs for 3 x 8 MiB AFC puts + 4 queries' % (time.time() - t0))
    procs.stop(syslog)
    report('syslog while concurrent', 0, '%d lines' % len(Path(out, 'syslog-conc.log').read_text(errors='replace').splitlines()))
    rc, text = tool(['ideviceinfo', '-k', 'ProductVersion'])
    report('lockdown after concurrency', rc, text)

    # --- 3. prototype agent RPCs --------------------------------------------
    print('\n== agent prototype', flush=True)
    deadline = time.monotonic() + 60
    while not r.itqmp.agent_alive(dev.qmp):
        assert time.monotonic() < deadline, 'baked agent never became ready'
        time.sleep(1)
    agent('ping')
    # Bootstrap only: the baked (shell-using) agent replaces itself with the prototype.
    assert agent('put', '/usr/local/bin/it_agent 755', Path(args.agent).read_bytes())[0] == 0
    assert agent('spawn', '', argv_body('/bin/launchctl', 'list'))[0] != 0  # ENOSYS: the old agent has no spawn
    try:
        agent('exec', 'launchctl stop com.qemu.it-agent', timeout=15)
    except Exception as e:  # the daemon we asked is the one that stops
        print(t(), 'restart request ended as expected:', e, flush=True)
    deadline = time.monotonic() + 60
    while True:
        try:
            if r.itqmp.agent_alive(dev.qmp) and agent('spawn', '', argv_body('/bin/launchctl', 'list'), timeout=20)[0] == 0:
                break
        except Exception as e:
            print(t(), 'waiting for the prototype agent:', e, flush=True)
        assert time.monotonic() < deadline, 'prototype agent did not take over'
        time.sleep(2)
    status, data = agent('spawn', '', argv_body('/bin/launchctl', 'list'))
    print(t(), 'launchctl list has SpringBoard:', b'com.apple.SpringBoard' in data, flush=True)
    assert agent('spawn', '', argv_body('/bin/launchctl', 'no-such-subcommand'))[0] != 0
    assert agent('sync')[0] == 0
    def sb_pid():
        _, listing = agent('spawn', '', argv_body('/bin/launchctl', 'list'))
        return [l for l in listing.decode(errors='replace').splitlines() if l.endswith('com.apple.SpringBoard')]
    before = sb_pid()
    syslog = procs.spawn(['idevicesyslog'], out + '/syslog-respring.log', env=r.mux_env(cfg))
    time.sleep(2)
    t0 = time.time()
    status, data = agent('spawn', '', argv_body('/bin/launchctl', 'stop', 'com.apple.SpringBoard'))
    assert status == 0, (status, data)
    back = None
    for _ in range(60):
        time.sleep(1)
        try:
            s, d = agent('frontmost', timeout=10)
        except Exception as e:
            print(t(), 'frontmost:', e, flush=True)
            continue
        if s == 0 and time.time() - t0 > 3:
            back = time.time() - t0
            break
    time.sleep(2)
    print(t(), 'launchctl SpringBoard row before %s, after %s' % (before, sb_pid()), flush=True)
    procs.stop(syslog)
    log = Path(out, 'syslog-respring.log').read_text(errors='replace').splitlines()
    sb = [line for line in log if 'SpringBoard' in line][:6]
    print(t(), 'SpringBoard answered again after %.1fs' % back if back else 'SpringBoard did not answer', flush=True)
    print('\n'.join('   syslog: ' + line[:170] for line in sb), flush=True)
    rc, text = tool(['ideviceinfo', '-k', 'ProductVersion'])
    report('lockdown after respring', rc, text)
    dev.qmp.shot(out + '/after-respring.ppm')
    r.to_png(out + '/after-respring.ppm', out + '/after-respring.png')
    print(t(), 'PROBE DONE', flush=True)
finally:
    procs.stop_all()
    shutil.rmtree(cfg.overlay, ignore_errors=True)
    if not args.keep:
        shutil.rmtree(out, ignore_errors=True)
