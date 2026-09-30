#!/usr/bin/env python3
"""Cold boot a stock restored store; judge actual lockdown identity, never brightness.

No guest additions, activation hook or host filesystem repair. Uses a private
NAND overlay/NOR clone and emulator-only USB through the libirecovery adapter.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]


def port():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--device', type=Path, required=True)
    parser.add_argument('--rom', type=Path, required=True)
    parser.add_argument('--libirecovery', type=Path, help='required only for --exit-recovery')
    parser.add_argument('--qemu', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--product-version', default='3.2.2')
    parser.add_argument('--timeout', type=int, default=120)
    parser.add_argument('--gdb', action='store_true', help='open an isolated loopback gdbstub and record its endpoint')
    parser.add_argument('--usb-enumeration-delay', type=int, default=0, help='diagnostic host attach delay in seconds')
    parser.add_argument('--exit-recovery', action='store_true', help='stock irecovery --normal, if recovery persists')
    parser.add_argument('--irecovery', default='irecovery')
    parser.add_argument('--ideviceinfo', default='ideviceinfo')
    parser.add_argument('--usbmuxd', default=os.path.expanduser('~/Developer/usbmuxd-qemu-ipad1-net/src/usbmuxd'))
    a = parser.parse_args()
    if a.exit_recovery and (not a.libirecovery or not (a.libirecovery / 'libirecovery-1.0.5.dylib').is_file()):
        parser.error('missing emulator transport adapter library')
    identity = json.loads((a.device / 'identity.json').read_text())
    a.out.mkdir()  # Keep old evidence intact.
    sock = a.out.resolve() / 'usb.sock'
    if len(str(sock).encode()) >= 104:
        parser.error('--out path too long for a Unix socket')
    for name in ('overlay', 'conf'):
        (a.out / name).mkdir()
    shutil.copyfile(a.device / 'nor.bin', a.out / 'nor.bin')
    def digest(path):
        with Path(path).open('rb') as stream:
            return hashlib.file_digest(stream, 'sha256').hexdigest()
    (a.out / 'inputs.json').write_text(json.dumps({
        'device': str(a.device.resolve()), 'qemu_sha256': digest(a.qemu),
        'rom_sha256': digest(a.rom), 'nor_sha256': digest(a.device / 'nor.bin'),
        'gid_sha256': digest(a.device / 'gid-blobs.bin'),
        'exit_recovery': a.exit_recovery, 'guest_additions': False,
        'usb_enumeration_delay': a.usb_enumeration_delay, 'usbmuxd_sha256': digest(a.usbmuxd),
    }, indent=2) + '\n')
    usb_port, mux_usb_port, mux_port = port(), port(), port()
    procs, streams = [], []
    def start(command, name, env=None):
        stream = (a.out / name).open('w')
        streams.append(stream)
        proc = subprocess.Popen(command, stdout=stream, stderr=subprocess.STDOUT, env=env)
        procs.append(proc)
        return proc
    env = os.environ | {'USBMUXD_SOCKET_ADDRESS': f'127.0.0.1:{mux_port}'}
    if a.exit_recovery:
        env |= {'IRECV_QEMU_SOCKET': str(sock), 'DYLD_LIBRARY_PATH': str(a.libirecovery.resolve())}
    try:
        start([a.usbmuxd, '-f', '-v', '-v', '-S', f'127.0.0.1:{mux_port}', '-P', 'NONE', '-C', str(a.out / 'conf')],
              'usbmuxd.log', os.environ | {'USBMUXD_QEMU_ADDR': f'127.0.0.1:{mux_usb_port}', 'USBMUXD_QEMU_DELAY': str(a.usb_enumeration_delay)})
        if a.exit_recovery:
            start([sys.executable, str(ROOT / 'imgtools/usb_recovery_bridge.py'), '--socket', str(sock),
                   '--port', str(usb_port), '--usbmuxd', f'127.0.0.1:{mux_usb_port}'], 'bridge.log')
            deadline = time.monotonic() + 10
            while not sock.exists():
                if time.monotonic() > deadline or any(p.poll() is not None for p in procs):
                    raise RuntimeError('USB bridge did not start')
                time.sleep(.05)
        else:
            # Canonical cold boot: usbmuxd alone owns EP0. No recovery poller.
            usb_port = mux_usb_port
        machine = (f'ipad1,bootrom={a.rom.resolve()},development-fuses=on,'
                   f'die-id={":".join(identity["die-id"])},gid-blobs={(a.device / "gid-blobs.bin").resolve()},'
                   f'nor-rw={(a.out / "nor.bin").resolve()},nand={(a.device / "nand").resolve()},'
                   f'nand-overlay={(a.out / "overlay").resolve()},usb-tcp-addr=127.0.0.1:{usb_port}')
        command = [str(a.qemu), '-machine', machine, '-display', 'none', '-audio', 'driver=none',
                   '-monitor', 'none', '-serial', f'file:{a.out}/serial.log']
        if a.gdb:
            endpoint = f'127.0.0.1:{port()}'
            command += ['-gdb', f'tcp:{endpoint}']
            (a.out / 'gdb-endpoint.txt').write_text(endpoint + '\n')
        (a.out / 'qemu-command.json').write_text(json.dumps(command, indent=2) + '\n')
        qemu = start(command, 'qemu.log')
        deadline = time.monotonic() + a.timeout
        exit_sent = False
        handed_off = False
        while time.monotonic() < deadline and qemu.poll() is None:
            if a.exit_recovery and not handed_off:
                try:
                    query = subprocess.run([a.irecovery, '-i', identity['unique-chip-id'], '--query'],
                                           env=env, capture_output=True, text=True, timeout=15)
                except subprocess.TimeoutExpired:
                    query = subprocess.CompletedProcess([], 1, '', 'descriptor poll timed out\n')
                with (a.out / 'recovery-query.log').open('a') as stream:
                    stream.write(query.stdout + query.stderr)
                if a.exit_recovery and not exit_sent and 'MODE: Recovery' in query.stdout:
                    normal = subprocess.run([a.irecovery, '-i', identity['unique-chip-id'], '--normal'],
                                            env=env, capture_output=True, text=True, timeout=15)
                    (a.out / 'exit-recovery.log').write_text(normal.stdout + normal.stderr)
                    if normal.returncode:
                        raise RuntimeError('stock recovery exit failed')
                    exit_sent = True
                handed_off = 'USB handed to usbmuxd-qemu' in (a.out / 'bridge.log').read_text(errors='replace')
            try:
                info = subprocess.run([a.ideviceinfo, '-s'], env=env, capture_output=True, text=True, timeout=10)
            except subprocess.TimeoutExpired:
                info = subprocess.CompletedProcess([], 1, '', 'lockdown query timed out\n')
            (a.out / 'identity.log').write_text(info.stdout + info.stderr)
            values = dict(line.split(': ', 1) for line in info.stdout.splitlines() if ': ' in line)
            if (info.returncode == 0 and values.get('ProductType') == 'iPad1,1' and
                    values.get('ProductVersion') == a.product_version and
                    values.get('SerialNumber') == identity['serial-number']):
                (a.out / 'result.json').write_text(json.dumps({'success': True, 'identity': values,
                    'exit_recovery': exit_sent, 'activation_state': values.get('ActivationState')}, indent=2) + '\n')
                print('PASS: stock restored cold boot; lockdown identity matches; ActivationState=' + values.get('ActivationState', 'unknown'))
                return
            time.sleep(.5)
        raise RuntimeError('stock lockdown identity did not match before the boot deadline; see logs')
    except Exception as error:
        (a.out / 'result.json').write_text(json.dumps({'success': False, 'error': str(error), 'exit_recovery': exit_sent if 'exit_sent' in locals() else False}, indent=2) + '\n')
        raise
    finally:
        for proc in reversed(procs):
            if proc.poll() is None:
                proc.terminate()
        for proc in reversed(procs):
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
        for stream in streams:
            stream.close()


if __name__ == '__main__':
    main()
