#!/usr/bin/env python3
"""Boot through iBoot on an isolated NAND overlay and capture the resulting UI."""
import argparse
from pathlib import Path
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'imgtools'))
import itqmp
from itqmp import QMP
itqmp.W, itqmp.H = 1024, 768


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--qemu', default=str(ROOT / 'build/qemu-system-arm'))
    ap.add_argument('--iboot', required=True)
    ap.add_argument('--nor', required=True)
    ap.add_argument('--nand', required=True)
    ap.add_argument('--out', required=True, type=Path)
    ap.add_argument('--seconds', type=int, default=180)
    ap.add_argument('--unlock', action='store_true', help='exercise slide-to-unlock after the first screenshot')
    a = ap.parse_args()
    a.out.mkdir(parents=True, exist_ok=True)
    serial, sock = a.out / 'serial.log', a.out / 'qmp'
    if len(str(sock.resolve())) > 100:
        ap.error('--out path is too long for a Unix socket')
    machine = f'ipad1,iboot={a.iboot},nor={a.nor},nand={a.nand},nand-overlay={a.out}/overlay'
    p = subprocess.Popen([a.qemu, '-machine', machine, '-display', 'none', '-monitor', 'none',
                          '-serial', f'file:{serial}', '-qmp', f'unix:{sock},server=on,wait=off'],
                         stdout=subprocess.DEVNULL, stderr=open(a.out / 'qemu.log', 'w'))
    start, next_shot, q = time.monotonic(), 15, None
    lit = False
    seen = set()
    try:
        q = QMP(str(sock), timeout=10)
        while p.poll() is None and time.monotonic() - start < a.seconds:
            elapsed = time.monotonic() - start
            log = serial.read_bytes().replace(b'\0', b'').decode(errors='replace')
            for marker in ('Uncompressed kernel cache', 'Radio not detected', 'iBoot version:',
                           'BSD root:', 'has started up.'):
                if marker in log and marker not in seen:
                    seen.add(marker)
                    print(f'{elapsed:6.1f}s {marker}', flush=True)
            if 'panic(cpu' in log or 'panic:' in log:
                raise RuntimeError('guest panic; see serial.log')
            if elapsed >= next_shot:
                dest = a.out / 'screen.ppm'
                q.cmd('screendump', filename=str(dest.resolve()))
                data = dest.read_bytes()
                bright = sum(x > 60 for x in data[100::13])
                if bright > 20000:
                    lit = True
                    print(f'{elapsed:6.1f}s lock screen lit ({bright} bright samples)', flush=True)
                    if a.unlock:
                        itqmp.swipe(q, 64, 290, 64, 710, steps=30, dt=0.03)
                        time.sleep(3)
                        q.cmd('screendump', filename=str((a.out / 'unlocked.ppm').resolve()))
                    break
                next_shot += 1
            time.sleep(1)
        if not lit:
            raise RuntimeError('did not reach a lit lock screen; see screen.ppm')
        if not {'Uncompressed kernel cache', 'iBoot version:', 'BSD root:', 'has started up.'} <= seen:
            raise RuntimeError('did not reach launchd through iBoot')
        print('PASS: iBoot loaded the kernel and device tree; root mounted, launchd started, and the lock screen lit.')
        print('Inspect screen captures to verify SpringBoard.')
    finally:
        if q:
            q.close()
        p.terminate()
        try:
            p.wait(timeout=10)
        except subprocess.TimeoutExpired:
            p.kill()
            p.wait()


if __name__ == '__main__':
    main()
