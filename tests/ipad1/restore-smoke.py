#!/usr/bin/env python3
"""Boot a stock restore ramdisk through SecureROM and emulated DFU/recovery USB.

macOS: uses an isolated libirecovery transport adapter and usbmuxd-qemu.
--erase additionally restores a disposable clone of the selected device.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'imgtools'))
from ipad1_boot import DEFAULT_DEVICE


def port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


def blank_nand(source, destination):
    """Preserve only measured geometry; no synthetic FTL or filesystem seed."""
    geometry = json.loads((source / 'geometry.json').read_text())
    fields = ('page_bytes', 'spare_bytes', 'pages_per_block', 'blocks_per_ce',
              'ce_per_bus', 'buses')
    if any(type(geometry.get(key)) is not int or geometry[key] <= 0 for key in fields):
        raise ValueError('invalid NAND geometry')
    if geometry['buses'] > 2 or geometry['ce_per_bus'] > 8:
        raise ValueError('NAND geometry exceeds controller capacity')
    size = ((geometry['page_bytes'] + geometry['spare_bytes']) *
            geometry['pages_per_block'] * geometry['blocks_per_ce'])
    destination.mkdir()
    (destination / 'geometry.json').write_text(json.dumps(geometry, indent=2) + '\n')
    for bus in range(geometry['buses']):
        for ce in range(geometry['ce_per_bus']):
            # Current page-store format defines untouched sparse holes as erased.
            with (destination / f'bus{bus}-ce{ce}.pages').open('xb') as stream:
                stream.truncate(size)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--device', type=Path, default=Path(DEFAULT_DEVICE))
    parser.add_argument('--rom', type=Path, required=True)
    parser.add_argument('--ipsw', type=Path, required=True)
    parser.add_argument('--libirecovery', type=Path, required=True, help='adapter build directory')
    parser.add_argument('--qemu', type=Path, default=ROOT / 'build/qemu-system-arm')
    parser.add_argument('--idevicerestore', default='idevicerestore')
    parser.add_argument('--usbmuxd', default=os.path.expanduser('~/Developer/usbmuxd-qemu-ipad1-net/src/usbmuxd'))
    parser.add_argument('--out', type=Path)
    parser.add_argument('--erase', action='store_true', help='restore a disposable APFS clone; source device is read-only')
    parser.add_argument('--blank-nand', action='store_true',
                        help='with --erase, start with geometry only, without generated FTL/filesystems')
    parser.add_argument('--timeout', type=int, default=900)
    a = parser.parse_args()
    if a.blank_nand and not a.erase:
        parser.error('--blank-nand requires --erase')
    identity = json.loads((a.device / 'identity.json').read_text())
    ecid = identity['unique-chip-id']
    for path in (a.rom, a.ipsw, a.device / 'gid-blobs.bin', a.libirecovery / 'libirecovery-1.0.5.dylib'):
        if not path.is_file():
            parser.error(f'missing {path}')
    out = a.out or Path(tempfile.mkdtemp(prefix='ipad-restore-'))
    if a.out:
        out.mkdir()  # Never overwrite another run.
    print(f'Artifacts: {out}', flush=True)
    usb_port, mux_usb_port, mux_port = port(), port(), port()
    bridge_socket = str(out / 'usb.sock')
    if len(bridge_socket.encode()) >= 104:
        parser.error('--out path too long for a Unix socket')
    machine = (f'ipad1,bootrom={a.rom.resolve()},development-fuses=on,'
               f'die-id={":".join(identity["die-id"])},'
               f'gid-blobs={(a.device / "gid-blobs.bin").resolve()},'
               f'usb-tcp-addr=127.0.0.1:{usb_port}')
    if a.erase:
        if a.blank_nand:
            blank_nand(a.device / 'nand', out / 'nand')
        else:
            subprocess.run(['cp', '-cR', str(a.device / 'nand'), str(out / 'nand')], check=True)
            subprocess.run(['chmod', '-R', 'u+w', str(out / 'nand')], check=True)
        nor = bytearray((a.device / 'nor.bin').read_bytes())
        # A factory restore target has identity/NVRAM but no boot images.
        # With no LLB to load, stock SecureROM enters DFU by itself.
        nor[0x8000:0xfc000] = b'\xff' * (0xfc000 - 0x8000)
        (out / 'nor.bin').write_bytes(nor)
        machine += f',nand={out / "nand"},nor-rw={out / "nor.bin"}'
    if os.environ.get('IPAD1_MACHINE_EXTRA'):   # e.g. iop-core=off, as boot-smoke.py takes it
        machine += ',' + os.environ['IPAD1_MACHINE_EXTRA']
    processes, logs = [], []
    def digest(path):
        with path.open('rb') as stream:
            return hashlib.file_digest(stream, 'sha256').hexdigest()
    (out / 'inputs.json').write_text(json.dumps({
        'blank_nand': a.blank_nand,
        'device': str(a.device.resolve()),
        'qemu_sha256': digest(a.qemu),
        'rom_sha256': digest(a.rom),
        'ipsw_sha256': digest(a.ipsw),
        'gid_blobs_sha256': digest(a.device / 'gid-blobs.bin'),
    }, indent=2) + '\n')

    def start(command, log, env=None):
        stream = (out / log).open('w')
        logs.append(stream)
        p = subprocess.Popen(command, stdout=stream, stderr=subprocess.STDOUT, env=env)
        processes.append(p)
        return p

    try:
        start([a.usbmuxd, '-f', '-v', '-S', f'127.0.0.1:{mux_port}', '-P', 'NONE', '-C', str(out / 'conf')],
              'usbmuxd.log', os.environ | {'USBMUXD_QEMU_ADDR': f'127.0.0.1:{mux_usb_port}', 'USBMUXD_QEMU_DELAY': '0'})
        start([sys.executable, str(ROOT / 'imgtools/usb_recovery_bridge.py'), '--socket', bridge_socket,
               '--port', str(usb_port), '--usbmuxd', f'127.0.0.1:{mux_usb_port}'], 'bridge.log')
        deadline = time.monotonic() + 10
        while not Path(bridge_socket).exists():
            if time.monotonic() > deadline or any(p.poll() is not None for p in processes):
                raise RuntimeError('USB bridge did not start; see logs')
            time.sleep(.05)
        start([str(a.qemu), '-machine', machine, '-display', 'none', '-audio', 'driver=none',
               '-monitor', 'none', '-serial', f'file:{out}/serial.log'], 'qemu.log')
        env = os.environ | {'IRECV_QEMU_SOCKET': bridge_socket,
                            'DYLD_LIBRARY_PATH': str(a.libirecovery.resolve()),
                            'USBMUXD_SOCKET_ADDRESS': f'127.0.0.1:{mux_port}'}
        command = [a.idevicerestore, '-c', '-e' if a.erase else '-z', '-y', '-i', ecid,
                   '--logfile', str(out / 'idevicerestore.log'), '-C', str(out / 'cache'), str(a.ipsw.resolve())]
        (out / 'command.json').write_text(json.dumps(command, indent=2))
        restore = start(command, 'restore.log', env)
        code = restore.wait(timeout=a.timeout)
        log = (out / 'restore.log').read_text(errors='replace')
        marker = 'Status: Restore Finished' if a.erase else 'Device is now in restore mode'
        success = code == 0 and marker in log
        if not success:
            raise RuntimeError(f'idevicerestore exited {code}; see {out / "restore.log"}')
        print('PASS: ' + ('blank-NAND erase restore' if a.blank_nand else
                          'erase restore' if a.erase else 'SecureROM → DFU → recovery → restore ramdisk'))
    finally:
        for p in reversed(processes):
            if p.poll() is None:
                p.terminate()
        for p in reversed(processes):
            try:
                p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                p.kill()
                p.wait()
        for stream in logs:
            stream.close()


if __name__ == '__main__':
    main()
