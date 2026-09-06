#!/usr/bin/env python3
"""Timer multiplier must match saved hardware semantics."""
from pathlib import Path
import os, socket, subprocess, tempfile, time, sys
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'imgtools'))
from itqmp import QMP
files = ROOT.parent / 'qemu-ios-files'
with tempfile.TemporaryDirectory(prefix='it-snapshot-modes-') as temporary:
    out = Path(temporary)
    for option, section in [('time-dilation', 'ipod_touch_timer')]:
        for enabled in ['1', '2']:
            for phase in range(3):
                mode = enabled if phase < 2 else ('2' if enabled == '1' else '1')
                qpath = str(out / 'qmp')
                tpath = str(out / 'qtest')
                argv = [str(ROOT / 'build-native14/qemu-build/qemu-system-arm'), '-S', '-M',
                    f'iPod-Touch,bootrom={files}/bootrom_240_4,nand={files}/nand,nor={files}/nor_n72ap.bin,nandrw={out}/overlay,{option}={mode}',
                    '-m', '128M', '-display', 'none', '-serial', 'null', '-monitor', 'none',
                    '-qmp', f'unix:{qpath},server=on,wait=off',
                    '-qtest', f'unix:{tpath},server=on,wait=off', '-qtest-log', str(out / 'qtest.log')]
                if phase: argv += ['-incoming', 'file:' + str(out / 'state')]
                with (out / 'qemu.log').open('w') as log:
                    child = subprocess.Popen(argv, stdout=log, stderr=log,
                        env={k:v for k,v in os.environ.items() if not k.startswith('IT_')} | {'IT_TIMER_TRACE':'1'})
                    q = None
                    try:
                        if phase == 2:
                            assert child.wait(timeout=20) != 0
                            assert section in (out / 'qemu.log').read_text()
                            print('PASS:', option, enabled, 'rejects incompatible restore', flush=True)
                            continue
                        deadline = time.monotonic() + 20
                        while not Path(qpath).exists():
                            assert child.poll() is None and time.monotonic() < deadline
                            time.sleep(.05)
                        q = QMP(qpath, timeout=10)
                        while q.cmd('query-status')['status'] == 'inmigrate':
                            assert time.monotonic() < deadline
                            time.sleep(.05)
                        with socket.socket(socket.AF_UNIX) as connection:
                            connection.settimeout(10)
                            connection.connect(tpath)
                            with connection.makefile('rwb', buffering=0) as wire:
                                def write(offset, value):
                                    wire.write(f'writel {0x3c700000 + offset:#x} {value:#x}\n'.encode())
                                    assert wire.readline().startswith(b'OK')
                                if not phase:
                                    write(0xa8, 10000)
                                # Restore keeps bcount1; repeating CONFIG must reproduce
                                # the original interval using identical startup dilation.
                                write(0xa0, 0)
                        expected = f'tick_interval={1000000 * int(enabled)} ns'
                        assert expected in (out / 'qemu.log').read_text()
                        print('PASS:', option, enabled, 'restored reprogram' if phase else 'initial interval', flush=True)
                        if not phase:
                            q.cmd('migrate', uri='file:' + str(out / 'state'))
                            while True:
                                state = q.cmd('query-migrate')
                                assert state['status'] != 'failed', state
                                if state['status'] == 'completed': break
                                assert time.monotonic() < deadline
                                time.sleep(.05)
                        q.cmd('quit')
                        assert child.wait(timeout=10) == 0
                    finally:
                        if q: q.close()
                        if child.poll() is None: child.kill(); child.wait()
                        Path(qpath).unlink(missing_ok=True)
                        Path(tpath).unlink(missing_ok=True)
