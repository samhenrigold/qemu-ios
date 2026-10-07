#!/usr/bin/env python3
"""Capture early 7E18 boot frames in a fresh overlay; stop the owned guest afterward.

Use --keep-running to retain it for debugging. QEMU/NAND/NOR/SEED_OVL and
SHOT_INTERVAL environment overrides remain supported. Existing output directories
are refused so neither evidence nor an older overlay is overwritten.
"""
import argparse
import json
import math
import os
from pathlib import Path
import re
import shutil
import subprocess
import time
import uuid

import itqmp


def positive(value):
    value = float(value)
    if not math.isfinite(value) or value <= 0:
        raise argparse.ArgumentTypeError("must be finite and positive")
    return value


def main(arguments=None):
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('port', type=int)
    parser.add_argument('tag')
    parser.add_argument('duration', type=positive, nargs='?', default=70)
    parser.add_argument('--out', type=Path)
    parser.add_argument('--files', type=Path, default=root.parent/'qemu-ios-files')
    parser.add_argument('--keep-running', action='store_true')
    args = parser.parse_args(arguments)
    if not 1 <= args.port <= 65535 or not re.fullmatch(r'[A-Za-z0-9_-]+', args.tag):
        parser.error('use a valid TCP port and a tag containing letters, digits, underscore or hyphen')
    interval = positive(os.environ.get('SHOT_INTERVAL', '1.2'))
    output = (args.out or root/'imgtools'/('boot_'+args.tag)).resolve()
    output.mkdir(parents=True, exist_ok=False)
    overlay = output/'nandrw'
    if os.environ.get('SEED_OVL'):
        shutil.copytree(os.environ['SEED_OVL'], overlay)
    else:
        overlay.mkdir()
    files = args.files.resolve()
    firmware = files/'ios3'
    env = dict(os.environ)
    env.update(IT_DIRECT_IBOOT=str(firmware/'iBoot.bin'),
               IT_INJECT_DT=str(firmware/'DeviceTree.nowdt.bin'), IT_WDT_NORESET='1')
    for key, value in os.environ.items():
        if key.startswith('IT_SET_'):
            env[key[len('IT_SET_'):]] = value
        elif key.startswith('IT_UNSET_'):
            env.pop(value, None)
    machine = {'bootrom':files/'bootrom_240_4',
               'nand':os.environ.get('NAND', str(files/'nand-agent-v4')),
               'nor':os.environ.get('NOR', str(firmware/'nor_7E18.bin')), 'nandrw':overlay}
    name = 'bootshot-'+uuid.uuid4().hex
    command = [os.environ.get('QEMU', str(root/'build-native14/qemu-build/qemu-system-arm')),
               '-name', name, '-M', 'iPod-Touch,'+','.join(key+'='+str(value).replace(',', ',,') for key,value in machine.items()),
               '-m', '128M', '-display', 'none', '-serial', 'file:'+str(output/'serial.log'),
               '-qmp', 'tcp:127.0.0.1:%d,server=on,wait=off'%args.port]
    rows, qmp, complete = [], None, False
    started = time.monotonic()
    with (output/'qemu.log').open('wb') as log:
        process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT, env=env)
        try:
            qmp = itqmp.QMP('127.0.0.1', args.port, timeout=30, read_timeout=10)
            if qmp.cmd('query-name').get('name') != name:
                raise RuntimeError('QMP port belongs to another guest')
            print('QMP connected; owned QEMU pid', process.pid, flush=True)
            while time.monotonic()-started < args.duration:
                if process.poll() is not None:
                    raise RuntimeError('QEMU exited during capture: %s'%process.returncode)
                index, elapsed = len(rows), time.monotonic()-started
                path = output/('f%03d.ppm'%index)
                qmp.shot(path)
                _, _, pixels = itqmp.read_ppm(path)
                high, nonzero = max(pixels), sum(value != 0 for value in pixels)
                rows.append((index, elapsed, high, nonzero, len(pixels)))
                print('f%03d t+%.1fs max=%d nonzero=%d'%(index, elapsed, high, nonzero), flush=True)
                time.sleep(min(interval, max(0, args.duration-(time.monotonic()-started))))
            complete = True
        finally:
            if qmp:
                qmp.close()
            if complete and args.keep_running and process.poll() is None:
                (output/'qemu.pid').write_text(str(process.pid)+'\n')
                print('Leaving owned QEMU pid %d running (--keep-running)'%process.pid)
            elif process.poll() is None:
                # This is a disposable capture overlay, not a clean-shutdown test.
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill(); process.wait(timeout=5)
            (output/'frames.json').write_text(json.dumps(rows, indent=2)+'\n')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
