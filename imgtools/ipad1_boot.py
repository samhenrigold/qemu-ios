"""Shared iPad boot selection for test runners. Real iBoot is the default."""
import json
import os
from pathlib import Path

DEFAULT_DEVICE = os.path.expanduser('~/Developer/qemu-ios-files/ipad1/repro/default-iboot')
# -M name -> scanout pixels, for the runners' absolute touch coordinates
MACHINES = {'ipad1': (1024, 768), 'iPod-Touch-4G': (640, 960)}


def add_arguments(parser):
    parser.add_argument('--machine', default='ipad1', choices=MACHINES, help='the A4 board\'s -M name')
    parser.add_argument('--device', default=DEFAULT_DEVICE, help='device.py output (iBoot, NOR, keys, NAND)')
    parser.add_argument('--kboot', help='explicit direct-kernel bring-up fallback')
    parser.add_argument('--bootrom', help='explicit SecureROM boot for restore validation')
    parser.add_argument('--development-fuses', action='store_true', help='engineering fuse policy for unpersonalized stock firmware')
    parser.add_argument('--iboot', help='override device iBoot')
    parser.add_argument('--nor', help='override device NOR')
    parser.add_argument('--gid-blobs', help='override device catalog key data')


def kboot_nor(nor, writable_dir):
    """',nor-rw=COPY' for kboot with a NOR (effaceable, e.g. a 4.x keybag): a private writable copy."""
    if not (nor and writable_dir):
        return ''
    import shutil
    target = Path(writable_dir) / 'nor.bin'
    target.parent.mkdir(parents=True, exist_ok=True)
    if not target.exists():
        shutil.copyfile(nor, target)
    return f',nor-rw={target}'


def die_id(device):
    identity = device / 'identity.json'
    if identity.exists():
        with identity.open() as f:
            die = json.load(f).get('die-id')
        if die:
            return ',die-id=' + ':'.join(die)
    return ''


def boot_options(cfg, writable_dir=None):
    if sum(bool(getattr(cfg, name, None)) for name in ('kboot', 'iboot', 'bootrom')) > 1:
        raise ValueError('select only one of --kboot, --iboot, --bootrom')
    device = Path(getattr(cfg, 'device', DEFAULT_DEVICE))
    if not getattr(cfg, 'kboot', None) and not getattr(cfg, 'iboot', None) and not getattr(cfg, 'bootrom', None):
        # A FirmwareKit kboot device (n81ap: boot_strategy kboot): its kboot.bin and nor.bin.
        lock = device / 'device.lock.json'
        if lock.is_file() and json.loads(lock.read_text()).get('boot_strategy') == 'kboot':
            nor = getattr(cfg, 'nor', None) or ((device / 'nor.bin').is_file() and str(device / 'nor.bin'))
            return f'kboot={device / "kboot.bin"}' + kboot_nor(nor, writable_dir) + die_id(device)
    if getattr(cfg, 'kboot', None):
        return f'kboot={cfg.kboot}' + kboot_nor(getattr(cfg, 'nor', None), writable_dir)
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
    result = ','.join(f'{key}={path}' for key, path in paths.items()) + die_id(device)
    if getattr(cfg, 'development_fuses', False):
        result += ',development-fuses=on'
    return result
