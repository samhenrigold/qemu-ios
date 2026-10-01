#!/usr/bin/env python3
"""Verify stock N72 SecureROM/iBSS USB identity on disposable storage.

Requires the emulator-only libirecovery adapter (IRECV_QEMU_SOCKET). Never
opens physical USB and never writes the prepared device's NOR or NAND.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('device', 'rom', 'ibss', 'libirecovery', 'out'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--qemu', type=Path, default=ROOT / 'build/qemu-system-arm')
    parser.add_argument('--irecovery', default='irecovery')
    a = parser.parse_args()
    identity = json.loads((a.device / 'identity.json').read_text())
    ecid = identity.get('unique-chip-id')
    if ecid is None:
        # Compatibility with immutable bases created before ECID provisioning.
        ecid = hex(int.from_bytes(hashlib.sha256(identity['seed'].encode()).digest()[20:25], 'big') | 1)
    ecid = int(ecid, 0)
    if not 0 < ecid < 1 << 42:
        parser.error('N72 unit ECID must be nonzero and fit 42 bits')
    for path in (a.rom, a.ibss, a.qemu, a.device / 'nor.bin', a.device / 'gid-blobs.bin',
                 a.libirecovery / 'libirecovery-1.0.5.dylib'):
        if not path.is_file():
            parser.error(f'missing {path}')
    a.out.mkdir()  # Refuse to replace evidence from an earlier run.
    (a.out / 'overlay').mkdir()
    with socket.socket() as listener:
        listener.bind(('127.0.0.1', 0))
        port = listener.getsockname()[1]
    bridge_socket = a.out.resolve() / 'usb.sock'
    if len(str(bridge_socket).encode()) >= 104:
        parser.error('--out path too long for a Unix socket')
    (a.out / 'nor.bin').write_bytes(b'\xff' * (a.device / 'nor.bin').stat().st_size)
    inputs = {'ecid': hex(ecid), 'device': str(a.device.resolve())}
    for name, path in (('qemu', a.qemu), ('rom', a.rom), ('ibss', a.ibss),
                       ('gid_blobs', a.device / 'gid-blobs.bin')):
        with path.open('rb') as stream:
            inputs[name + '_sha256'] = hashlib.file_digest(stream, 'sha256').hexdigest()
    (a.out / 'inputs.json').write_text(json.dumps(inputs, indent=2) + '\n')
    processes, streams = [], []

    def start(command, log):
        stream = (a.out / log).open('w')
        streams.append(stream)
        process = subprocess.Popen(command, stdout=stream, stderr=subprocess.STDOUT)
        processes.append(process)
        return process

    def run(arguments, log):
        result = subprocess.run([a.irecovery, *arguments], env=env, capture_output=True,
                                text=True, timeout=60)
        (a.out / log).write_text(result.stdout + result.stderr)
        result.check_returncode()
        return result.stdout

    try:
        bridge = start([sys.executable, str(ROOT / 'imgtools/usb_recovery_bridge.py'),
                        '--socket', str(bridge_socket), '--port', str(port)], 'bridge.log')
        deadline = time.monotonic() + 10
        while not bridge_socket.exists():
            if bridge.poll() is not None or time.monotonic() > deadline:
                raise RuntimeError('USB bridge did not start')
            time.sleep(.05)
        machine = (f'iPod-Touch,ecid={hex(ecid)},bootrom={a.rom.resolve()},'
                   f'nor={a.out.resolve()}/nor.bin,nand={a.device.resolve()}/nand,'
                   f'nandrw={a.out.resolve()}/overlay,usb-attached=on,aes-uid=engine,'
                   f'gid-blobs={a.device.resolve()}/gid-blobs.bin,usb-tcp-addr=127.0.0.1:{port}')
        command = [str(a.qemu), '-machine', machine, '-display', 'none',
                   '-audio', 'driver=none', '-monitor', 'none',
                   '-serial', f'file:{a.out.resolve()}/serial.log']
        (a.out / 'command.json').write_text(json.dumps(command, indent=2) + '\n')
        start(command, 'qemu.log')
        # The current TCP bridge can enumerate before ROM startup has settled.
        # Keep that known transport race separate from the ECID/fuse contract.
        time.sleep(3)
        env = os.environ | {'IRECV_QEMU_SOCKET': str(bridge_socket),
                            'DYLD_LIBRARY_PATH': str(a.libirecovery.resolve())}
        for mode, log in (('DFU', 'dfu.log'), ('Recovery', 'ibss.log')):
            deadline = time.monotonic() + 30
            while True:
                try:
                    output = run(['--query'], log)
                except subprocess.CalledProcessError:
                    output = ''  # A real USB disconnect during handoff is expected.
                if f'MODE: {mode}' in output and f'0x{ecid:016x}' in output.lower():
                    break
                if time.monotonic() > deadline:
                    raise RuntimeError(f'guest did not enumerate {mode} with unit ECID; see {log}')
                time.sleep(.2)
            if mode == 'DFU':
                run(['-f', str(a.ibss.resolve())], 'upload.log')
                # Tight descriptor polling during manifestation is separately
                # unqualified; allow stock firmware's USB reinitialization.
                time.sleep(3)
        print('PASS: stock SecureROM → stock iBSS; unit ECID matches in both USB descriptors')
    finally:
        for process in reversed(processes):
            if process.poll() is None:
                process.terminate()
        for process in reversed(processes):
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        for stream in streams:
            stream.close()


if __name__ == '__main__':
    main()
