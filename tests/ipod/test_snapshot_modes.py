#!/usr/bin/env python3
"""Decoder/compositor startup modes must match saved hardware semantics."""
from pathlib import Path
import os, subprocess, tempfile, time, sys
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'imgtools'))
from itqmp import QMP
files = ROOT.parent / 'qemu-ios-files'
with tempfile.TemporaryDirectory(prefix='it-snapshot-modes-') as temporary:
    out = Path(temporary)
    for option, section in [('mpvd-decode', 'ipod_touch_mpvd'), ('lcd-planes', 'ipod_touch_lcd')]:
        for enabled in ['on', 'off']:
            for phase in range(3):
                mode = enabled if phase < 2 else ('off' if enabled == 'on' else 'on')
                qpath = str(out / 'qmp')
                argv = [str(ROOT / 'build-native14/qemu-build/qemu-system-arm'), '-S', '-M',
                    f'iPod-Touch,bootrom={files}/bootrom_240_4,nand={files}/nand,nor={files}/nor_n72ap.bin,nandrw={out}/overlay,{option}={mode}',
                    '-m', '128M', '-display', 'none', '-serial', 'null', '-monitor', 'none',
                    '-qmp', f'unix:{qpath},server=on,wait=off']
                if phase: argv += ['-incoming', 'file:' + str(out / 'state')]
                with (out / 'qemu.log').open('w') as log:
                    child = subprocess.Popen(argv, stdout=log, stderr=log,
                        env={k:v for k,v in os.environ.items() if not k.startswith('IT_')})
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
