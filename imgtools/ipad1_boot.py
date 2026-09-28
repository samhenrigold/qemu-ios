"""Shared iPad boot selection for test runners. Real iBoot is the default."""
import json
import os
from pathlib import Path

DEFAULT_DEVICE = os.path.expanduser('~/Developer/qemu-ios-files/ipad1/repro/default-iboot')


def add_arguments(parser):
    parser.add_argument('--device', default=DEFAULT_DEVICE, help='device.py output (iBoot, NOR, keys, NAND)')
    parser.add_argument('--kboot', help='explicit direct-kernel bring-up fallback')
    parser.add_argument('--bootrom', help='explicit SecureROM boot for restore validation')
    parser.add_argument('--development-fuses', action='store_true', help='engineering fuse policy for unpersonalized stock firmware')
    parser.add_argument('--iboot', help='override device iBoot')
    parser.add_argument('--nor', help='override device NOR')
    parser.add_argument('--gid-blobs', help='override device catalog key data')


def boot_options(cfg, writable_dir=None):
    if sum(bool(getattr(cfg, name, None)) for name in ('kboot', 'iboot', 'bootrom')) > 1:
        raise ValueError('select only one of --kboot, --iboot, --bootrom')
    if getattr(cfg, 'kboot', None):
        return f'kboot={cfg.kboot}'
    device = Path(getattr(cfg, 'device', DEFAULT_DEVICE))
    paths = {key: Path(getattr(cfg, key.replace('-', '_'), None) or device / filename)
             for key, filename in [('iboot', 'iBoot.bin'), ('nor', 'nor.bin'), ('gid-blobs', 'gid-blobs.bin')]}
    if getattr(cfg, 'bootrom', None):
        del paths['iboot']
        paths['bootrom'] = Path(cfg.bootrom)
    for key, path in paths.items():
        if not path.is_file():
            raise ValueError(f'{key}: missing {path}; create an iPad with device.py or select --device')
    if writable_dir:
        # Preserve the private NOR on repeated boots, just like the NAND overlay.
        import shutil
        target = Path(writable_dir) / 'nor.bin'
        target.parent.mkdir(parents=True, exist_ok=True)
        if not target.exists():
            shutil.copyfile(paths['nor'], target)
        paths['nor-rw'] = target
        del paths['nor']
    result = ','.join(f'{key}={path}' for key, path in paths.items())
    identity = device / 'identity.json'
    if identity.exists():
        with identity.open() as f:
            die = json.load(f).get('die-id')
        if die:
            result += ',die-id=' + ':'.join(die)
    if getattr(cfg, 'development_fuses', False):
        result += ',development-fuses=on'
    return result
