#!/usr/bin/env python3
"""Native QOM configuration checks; each paused machine owns a temporary overlay."""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'imgtools'))
from itqmp import QMP

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--qemu', type=Path, default=ROOT / 'build-native14/qemu-build/qemu-system-arm')
parser.add_argument('--files', type=Path, default=ROOT.parent / 'qemu-ios-files')
args = parser.parse_args()
base_env = {k: v for k, v in os.environ.items() if not k.startswith('IT_')}
cases = [
    ('default', {}, '', (1,)),
    ('alias', {'IT_TIME_DILATION':'0x2'}, '', (2,)),
    ('explicit', {'IT_TIME_DILATION':'bad'}, ',time-dilation=3', (3,)),
    ('maximum', {}, ',time-dilation=1000000', (1000000,)),
    ('zero', {'IT_TIME_DILATION':'0'}, '', None),
    ('negative', {'IT_TIME_DILATION':'-1'}, '', None),
    ('overflow', {'IT_TIME_DILATION':'18446744073709551616'}, '', None),
    ('empty', {'IT_TIME_DILATION':''}, '', None),
    ('junk', {'IT_TIME_DILATION':'2junk'}, '', None),
    ('invalid-property', {}, ',time-dilation=1000001', None),
]
for label, overrides, options, expected in cases:
    with tempfile.TemporaryDirectory(prefix='it-time-config-') as tmp:
        out = Path(tmp)
        sock = str(out / 'qmp')
        machine = (f'iPod-Touch,bootrom={args.files}/bootrom_240_4,nand={args.files}/nand,'
                   f'nor={args.files}/nor_n72ap.bin,nandrw={out}/overlay' + options)
        with (out / 'qemu.log').open('w+') as log:
            child = subprocess.Popen([str(args.qemu), '-S', '-M', machine, '-m', '128M',
                '-display', 'none', '-serial', 'null', '-monitor', 'none',
                '-qmp', f'unix:{sock},server=on,wait=off'],
                stdout=log, stderr=subprocess.STDOUT, env=base_env | overrides)
            q = None
            try:
                if expected is None:
                    assert child.wait(timeout=15) != 0, label
                    log.seek(0)
                    message = log.read()
                    assert ('IT_TIME_DILATION must be' in message or
                            'time-dilation' in message), message
                else:
                    deadline = time.monotonic() + 15
                    while not Path(sock).exists():
                        assert child.poll() is None and time.monotonic() < deadline, label
                        time.sleep(.05)
                    q = QMP(sock, timeout=10)
                    for prop, value in zip(('time-dilation',), expected):
                        assert q.cmd('qom-get', path='/machine', property=prop) == value, label
                        try:
                            q.cmd('qom-set', path='/machine', property=prop, value=value)
                        except Exception as error:
                            assert 'before the machine starts' in str(error), str(error)
                        else:
                            raise AssertionError('runtime mutation accepted: ' + prop)
                    q.cmd('quit')
                    assert child.wait(timeout=10) == 0
                print('PASS:', label, flush=True)
            finally:
                if q:
                    q.close()
                if child.poll() is None:
                    child.terminate()
                    try:
                        child.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        child.kill()
                        child.wait()
