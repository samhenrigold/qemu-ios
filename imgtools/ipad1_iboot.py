#!/usr/bin/env python3
"""Prepare an iPad NOR and pattern-patched iBoot for booting an IPSW kernelcache.

No firmware is distributed by this tool. Supply decrypted iBoot and the IPSW's
all_flash directory, plus iBoot32Patcher (e.g. from Legacy-iOS-Kit). The RSA patch
permits unpersonalized IPSW images; this is not a cryptographically verified boot.
The kernelcache must also be installed in the system volume (ipad1_rootfs.py
build --kernelcache). The direct-kernel machine path is unaffected.
"""
import argparse
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import zlib

from build_nor import build, K48_ORDER, S5L8930_UID_KEY
from ipad1_kboot import DEFAULT_BOOT_ARGS, IDENTITY_FILE, load_identity, identity_dt


def chrp_header(signature, name, size):
    if size % 16 or size < 16:
        raise ValueError('CHRP partition must be aligned to 16 bytes')
    hdr = bytearray(struct.pack('<BBH12s', signature, 0, size // 16, name.encode()))
    total = sum(hdr)
    while total > 255:
        total = (total & 255) + (total >> 8)
    hdr[1] = total
    return hdr


def nvram_bank(values):
    bank = bytearray(8192)
    bank[:16] = chrp_header(0x5a, 'nvram', 32)
    struct.pack_into('<I', bank, 20, 1)  # generation
    bank[32:48] = chrp_header(0x70, 'common', 2048)
    data = b'\0'.join((k + '=' + v).encode('ascii') for k, v in values.items()) + b'\0\0'
    if len(data) > 2032 or any('\0' in k + v for k, v in values.items()):
        raise ValueError('invalid NVRAM variables')
    bank[48:48 + len(data)] = data
    bank[2080:2096] = chrp_header(0x7f, 'free', 8192 - 2080)
    struct.pack_into('<I', bank, 16, zlib.adler32(bank[20:]))
    return bank


def nor_base(identity, boot_args):
    nor = bytearray(1024 * 1024)
    struct.pack_into('<4sIIII', nor, 0, b'2GMI', 64, 0, 0x200, 0x3d00)
    struct.pack_into('<I', nor, 0x30, zlib.crc32(nor[:0x30]))
    # iBoot's SysCfg reader uses 20-byte records, with a 16-byte inline value.
    entries = {'Mod#': identity['model-number'], 'Regn': identity['region-info'],
               'SrNm': identity['serial-number'], 'MLB#': identity['mlb-serial-number']}
    struct.pack_into('<4sIIIII', nor, 0x4000, b'gfCS', 24 + 20 * len(entries),
                     8192, 0x10001, 0, len(entries))
    for i, (key, value) in enumerate(entries.items()):
        value = value.encode('ascii')
        if len(value) > 16 or b'\0' in value:
            raise ValueError(f'invalid SysCfg value for {key}')
        struct.pack_into('<4s16s', nor, 0x4018 + i * 20, key[::-1].encode(), value)
    values = {'debug-uarts': '1', 'auto-boot': 'true', 'boot-command': 'fsboot',
              'boot-args': boot_args, 'wifiaddr': identity['wifi-address'],
              'btaddr': identity['bluetooth-address']}
    bank = nvram_bank(values)
    nor[0xfc000:0xfe000] = bank
    nor[0xfe000:] = bank
    return nor


def prepare(iboot, all_flash, patcher, out, identity, boot_args):
    out.mkdir(parents=True, exist_ok=True)
    patched = out / 'iBoot.bin'
    if patched.resolve() == iboot.resolve():
        raise ValueError('output must not overwrite the input firmware')
    # -a (environment boot args) is broken for 817.29 in the bundled patcher.
    # -b locates its string and instruction references and works on this build.
    command = [str(patcher), str(iboot), str(patched), '--rsa', '--debug', '-b', boot_args]
    patched.unlink(missing_ok=True)
    result = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (out / 'patch.log').write_text(result.stdout)
    if (result.returncode not in (0, 1) or not patched.exists()
            or patched.stat().st_size != iboot.stat().st_size
            or patched.read_bytes() == iboot.read_bytes()):
        raise RuntimeError(f'iBoot32Patcher failed; see {out / "patch.log"}')
    with tempfile.TemporaryDirectory(prefix='ipad1-nor-') as tmp:
        base = Path(tmp) / 'base.bin'
        base.write_bytes(nor_base(identity, boot_args))
        build(str(base), str(all_flash), str(out / 'nor.bin'), K48_ORDER,
              uid_key=S5L8930_UID_KEY)
    (out / 'identity.json').write_text(json.dumps(identity, indent=2) + '\n')
    print(f'Prepared {patched} and {out / "nor.bin"}; image signatures are bypassed.')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--iboot', required=True, type=Path)
    ap.add_argument('--all-flash', required=True, type=Path)
    ap.add_argument('--patcher', required=True, type=Path)
    ap.add_argument('--out', required=True, type=Path)
    ap.add_argument('--identity', type=Path, default=Path(IDENTITY_FILE), help='identity.json (ipad1_kboot.py)')
    ap.add_argument('--boot-args', default=DEFAULT_BOOT_ARGS)
    a = ap.parse_args()
    ident = load_identity(str(a.identity))
    identity = {**identity_dt(ident)[0], 'wifi-address': ident['wifi-mac'], 'bluetooth-address': ident['bt-mac']}
    prepare(a.iboot, a.all_flash, a.patcher, a.out, identity, a.boot_args)


if __name__ == '__main__':
    main()
