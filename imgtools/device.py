#!/usr/bin/env python3
"""Compatibility entry point: device creation is implemented by Swift FirmwareKit.

Preferred: device.py create --id BOARD-BUILD --ipsw IPSW --out DIR
Legacy:    device.py create MANIFEST DIR
Set FIRMWAREKIT to the executable and FIRMWAREKIT_CATALOG to the app catalog.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess


def sha(path, algo='sha256'):
    """Retained for archived format-oracle imports, not preparation."""
    h = hashlib.new(algo)
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 22), b''):
            h.update(chunk)
    return h.hexdigest()


def command(a):
    catalog = Path(a.catalog).expanduser().resolve()
    document = json.loads(catalog.read_text())
    if document.get('format') != 1:
        raise ValueError('unsupported catalog format')
    seed = a.seed
    if a.manifest:
        if not a.outdir or a.id or a.ipsw or a.out:
            raise ValueError('use MANIFEST DIR or --id/--ipsw/--out')
        m = json.loads(Path(a.manifest).expanduser().read_text())
        if m.get('format') != 1:
            raise ValueError('unsupported legacy manifest format')
        entry_id = m['board'] + '-' + m['build']
        ipsw = m['ipsw']['path']
        out = a.outdir
        seed = seed or m['identity']['seed']
    else:
        if a.outdir or not all((a.id, a.ipsw, a.out)):
            raise ValueError('--id, --ipsw and --out are required')
        entry_id, ipsw, out = a.id, a.ipsw, a.out
        m = None
    entries = [e for e in document['entries'] if e['id'] == entry_id]
    if len(entries) != 1:
        raise ValueError('catalog must contain exactly one entry named ' + entry_id)
    entry = entries[0]
    if m:
        # The manifest is only an input locator. Reject conflicting policy;
        # keys, activation, storage geometry and edits belong to the catalog.
        if m['ipsw']['sha1'] != entry['source']['sha1'] or m['product_type'] != entry['product_type']:
            raise ValueError('legacy manifest identity differs from catalog')
        if (m.get('activation') or {}).get('hook'):
            raise ValueError('activation is automatic; custom activation hooks are retired')
        recipe = entry['recipe']
        for name in ('storage', 'system_mib', 'data_size'):
            if name in m and m[name] != recipe[name]:
                raise ValueError('legacy manifest ' + name + ' differs from catalog')
        for name, value in m.get('options', {}).items():
            if recipe['options'].get(name) != value:
                raise ValueError('legacy manifest option ' + name + ' differs from catalog; use --id')
    executable = a.firmwarekit or shutil.which('firmwarekit')
    if not executable:
        raise ValueError('set FIRMWAREKIT to the Swift firmwarekit executable')
    argv = [str(Path(executable).expanduser()) if '/' in executable else executable, 'create',
            '--catalog', str(catalog), '--id', entry_id,
            '--ipsw', str(Path(ipsw).expanduser()), '--out', str(Path(out).expanduser())]
    for name, value in [('seed', seed), ('helper', a.helper), ('guest-tools', a.guest_tools),
                        ('cache', a.cache), ('sibling-entry', a.sibling_entry), ('sibling-ipsw', a.sibling_ipsw)]:
        if value:
            argv += ['--' + name, str(Path(value).expanduser()) if name != 'seed' else value]
    if a.gl_test:
        argv += ['--gl-test']
    return argv


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    sub = ap.add_subparsers(dest='cmd', required=True)
    c = sub.add_parser('create')
    c.add_argument('manifest', nargs='?')
    c.add_argument('outdir', nargs='?')
    for name in ('id', 'ipsw', 'out', 'seed', 'helper', 'guest-tools', 'cache', 'sibling-entry', 'sibling-ipsw'):
        c.add_argument('--' + name)
    c.add_argument('--catalog', default=os.environ.get('FIRMWAREKIT_CATALOG',
                   '~/Developer/LightTouchMac-multidevice/LightTouchMac/Resources/firmware-catalog.json'))
    c.add_argument('--firmwarekit', default=os.environ.get('FIRMWAREKIT'))
    c.add_argument('--gl-test', action='store_true')
    a = ap.parse_args()
    try:
        argv = command(a)
    except (ValueError, KeyError, OSError) as e:
        ap.error(str(e))
    raise SystemExit(subprocess.call(argv))


if __name__ == '__main__':
    main()
