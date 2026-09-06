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
parser.add_argument('--firmware', action='store_true', help='verify 7E18 profile and legacy MBX read isolation')
parser.add_argument('--orientation', action='store_true', help='verify native UI rotation and respring recovery')
parser.add_argument('--typing', action='store_true', help='verify injected Notes and Harness input')
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
        result = r.guest_ssh(cfg, port, ['launchctl unload /System/Library/LaunchDaemons/com.qemu.it-pbd.plist; launchctl unload /System/Library/LaunchDaemons/com.qemu.it-agent.plist; chmod 755 /tmp/it_agent; /tmp/it_agent </dev/null >/tmp/it_agent.log 2>&1 &'])
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
    assert agent('getrange', '257 2048 /tmp/agent binary') == (0, body[257:2305])
    assert agent('exec', 'printf error >&2; exit 7') == (7, b'error')
    assert agent('settime', '1000000000')[0] == 0
    time.sleep(3)
    status, data = agent('exec', 'date +%s'); assert status == 0 and abs(int(data) - time.time()) < 5
    assert agent('exec', 'sleep 2; printf alive') == (0, b'alive')
    assert r.itqmp.agent_alive(d.qmp)
    assert agent('lockstatus')[0] == 0
    ok, detail = r.unlock(cfg, port, d); assert ok, detail
    assert agent('launch', 'com.apple.Preferences')[0] == 0
    time.sleep(2)
    status, data = agent('frontmost'); assert status == 0 and b'com.apple.Preferences' in data
    if args.firmware:
        regions=[(0x081953e0,6),(0x08324aa8,4),(0x08324a00,24),(0x08460000,200),(0x08460100,40)]
        def memory(label):
            data=[]
            for index,(address,size) in enumerate(regions):
                file=Path(out)/f'{label}-{index}.bin'
                d.qmp.cmd('pmemsave',val=address,size=size,filename=str(file))
                data.append(file.read_bytes())
            return data
        before=memory('before-mbx')
        assert before[0] != bytes.fromhex('7fee3f0f7047'), '7E18 already received the legacy clock patch'
        d.qmp.cmd('human-monitor-command',**{'command-line':'xp /1wx 0x3940000c'})
        assert memory('after-mbx')==before, 'legacy MBX read modified the 7E18 kernel'
        assert agent('ping')==(0,b'it_agent v1\n')
        print('PASS: native 7E18 boot and legacy MBX read preserve kernel clock/driver memory',flush=True)
    if args.orientation:
        assert agent('orientation') == (0,b'0\n')
        import plistlib, zipfile
        landscape = Path(out)/'Landscape.ipa'
        with zipfile.ZipFile(ROOT/'contrib/it-harness/build/Harness.ipa') as source, zipfile.ZipFile(landscape,'w') as target:
            for item in source.infolist():
                data=source.read(item)
                if item.filename.endswith('/Info.plist'):
                    info=plistlib.loads(data)
                    info['UIInterfaceOrientation']='UIInterfaceOrientationLandscapeRight'
                    data=plistlib.dumps(info)
                target.writestr(item,data)
        installed=r.run(['ideviceinstaller','install',str(landscape)],cfg,90)
        assert installed.returncode==0,installed
        assert agent('launch','com.qemuios.harness')[0]==0
        time.sleep(3)
        status,data=agent('frontmost');assert status==0 and b'com.qemuios.harness' in data,(status,data)
        deadline=time.monotonic()+20
        while True:
            status,data=agent('orientation')
            if status==0 and int(data) in (-90,90):break
            if time.monotonic()>=deadline:
                image=d.qmp.shot(str(Path(out)/'orientation-failed.ppm'));r.to_png(image,str(Path(out)/'orientation-failed.png'))
                raise AssertionError((status,data))
            time.sleep(0.5)
        d.qmp.home()
        deadline=time.monotonic()+15
        while agent('orientation') != (0,b'0\n'):
            assert time.monotonic()<deadline
            time.sleep(0.5)
        d.qmp.cmd('qom-set',path='/machine',property='accel-orientation',value=1)
        import errno
        assert agent('exec','killall -STOP SpringBoard')[0]==0
        try:
            assert agent('orientation')[0]==-errno.ETIMEDOUT
            assert agent('ping')==(0,b'it_agent v1\n')
        finally:
            assert agent('exec','killall -CONT SpringBoard')[0]==0
        assert agent('kill','SpringBoard')[0] == 0
        time.sleep(10)
        deadline=time.monotonic()+30
        while agent('orientation') != (0,b'0\n'):
            assert time.monotonic()<deadline
            time.sleep(1)
        print('PASS: native orientation follows landscape app, Home and a new SpringBoard server',flush=True)
    d.qmp.cmd("qom-set", path="/machine", property="pasteboard", value="Agent clipboard acceptance")
    deadline = time.monotonic() + 50
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
    if args.typing:
        import plistlib
        def checked(op, args='', body=b''):
            status, data = agent(op, args, body)
            assert status == 0, (op, status, data)
            return data
        checked('put', '/usr/lib/it_typein.dylib 755', (ROOT/'contrib/it-agent/it_typein.dylib').read_bytes())
        path = '/System/Library/LaunchDaemons/com.apple.SpringBoard.plist'
        job = plistlib.loads(checked('get', path))
        environment = job.setdefault('EnvironmentVariables', {})
        libraries = environment.get('DYLD_INSERT_LIBRARIES', '').split(':')
        environment['DYLD_INSERT_LIBRARIES'] = ':'.join(dict.fromkeys(
            [library for library in libraries if library] + ['/usr/lib/it_typein.dylib']))
        checked('put', path + ' 644', plistlib.dumps(job, fmt=plistlib.FMT_BINARY))
        preferences_path = '/var/mobile/Library/Preferences/com.apple.springboard.plist'
        preferences = plistlib.loads(checked('get', preferences_path))
        preferences.pop('SBDontLockEver', None)
        checked('put', preferences_path + ' 600', plistlib.dumps(preferences, fmt=plistlib.FMT_BINARY))
        checked('exec', 'chown 501:501 ' + preferences_path)
        checked('exec', 'launchctl unload ' + path + '; launchctl load ' + path)
        time.sleep(15)
        unlocked, detail = r.unlock(cfg, port, d)
        assert unlocked, detail
        checked('launch', 'com.apple.mobilenotes')
        time.sleep(4)
        assert b'NotesNavigationButton' in checked('uidump')
        # Native 7E18 Notes: inspected Add button in the top-right navigation bar.
        d.qmp.tap(299, 42)
        time.sleep(1)
        checked('type', body='Light Touch — café ✓\nNative typing works.'.encode())
        checked('backspace')
        r.itqmp.key(d.qmp, 'x')
        time.sleep(1)
        tree = checked('uidump')
        assert 'text: Light Touch — café ✓\nNative typing worksx'.encode() in tree, tree
        Path(out, 'notes-ui.txt').write_bytes(tree)
        r.to_png(d.qmp.shot(out + '/notes.ppm'), out + '/notes.png')
        harness = ROOT/'contrib/it-harness/build/Harness.ipa'
        installed = r.run(['ideviceinstaller', 'install', str(harness)], cfg, 90)
        assert installed.returncode == 0, installed
        checked('launch', 'com.qemuios.harness')
        time.sleep(4)
        assert b'UITextField' in checked('uidump')
        d.qmp.tap(220, 40)
        time.sleep(1)
        checked('type', body='café✓'.encode())
        checked('backspace')
        r.itqmp.key(d.qmp, 'z')
        time.sleep(1)
        tree = checked('uidump')
        assert 'text: http://10.0.2.2:8000/caféz'.encode() in tree, tree
        Path(out, 'harness-ui.txt').write_bytes(tree)
        r.to_png(d.qmp.shot(out + '/harness.ppm'), out + '/harness.png')
        r.itqmp.button(d.qmp, 'home', hold_ms=100)
        time.sleep(2)
        assert b'Home Screen' in checked('frontmost')
        tree = checked('uidump')
        assert tree, 'SpringBoard inspection returned no window'
        r.itqmp.key(d.qmp, 'q')
        time.sleep(1)
        # Home from the first icon page opens Spotlight on native 3.1.3.
        r.itqmp.button(d.qmp, 'home', hold_ms=100)
        time.sleep(2)
        tree = checked('uidump')
        Path(out, 'spotlight-before.txt').write_bytes(tree)
        checked('type', body=b'Agent spotlight')
        tree = checked('uidump')
        assert b'Agent spotlight' in tree and b'qAgent spotlight' not in tree, tree
        Path(out, 'spotlight-ui.txt').write_bytes(tree)
        r.itqmp.button(d.qmp, 'power', hold_ms=100)
        time.sleep(2)
        assert b'locked=1' in checked('lockstatus')
        assert agent('type', body=b'must not type')[0] == -13
        assert b'Lock Screen' in checked('frontmost')
        print('PASS: foreground typing, Spotlight, unfocused key discard and locked input rejection', flush=True)
        print('PASS: Notes and third-party UITextField Unicode, deletion, host keys and UI inspection', flush=True)
    print('PASS: native agent ping, root exec, binary stdin/files, status, clock, liveness', flush=True)
    assert d.powerdown(), 'guest shutdown not confirmed'
    if args.firmware:
        log=Path(d.dir,'qemu.log').read_text(errors='replace')
        assert '[FIRMWARE] detected build 7E18' in log
        if os.environ.get('IT_AMFI_ALLOW_TASKPORT'):
            assert '[IT_AMFI_ALLOW_TASKPORT] patched' in log
finally:
    if d.qmp: d.qmp.close()
    p.stop_all()
