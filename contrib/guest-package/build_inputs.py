#!/usr/bin/env python3
"""Actual guest sources and external build inputs; independent of emulator HEAD."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

COMPONENTS = ('armv6-toolchain it-gles gles-public it-agent it-instprogress it-media '
              'it-proxy it-status it-halt it-orientation ipad1-guest appsync it-boot '
              'it-pasteboard it-ethlink it-seal it-prefs it-keybag it-heading it-gyro '
              'it-cctest it-gltest it-msmquiet guest-package').split()


def digest(path):
    value = hashlib.sha256()
    with path.open('rb') as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b''):
            value.update(chunk)
    return value.hexdigest()


def sources(root):
    paths = []
    for component in COMPONENTS:
        for p in (root / 'contrib' / component).rglob('*'):
            if (not p.is_file() or '__pycache__' in p.parts or p.name.startswith('.')
                    or p.suffix in {'.md', '.o', '.pyc', '.cclog', '.ldlog', '.itpack'}
                    or p.name == 'gles_stubs.h'):
                continue
            with p.open('rb') as source:
                if source.read(4) in (b'\xce\xfa\xed\xfe', b'\xca\xfe\xba\xbe', b'\xcf\xfa\xed\xfe'):
                    continue
            paths.append(p)
    paths += list((root / 'tests/guest-package').glob('*.py'))
    paths += list((root / 'contrib/ios-app').glob('*.h'))
    paths += list((root / 'contrib/macos-app').glob('*.h'))
    paths += [root / n for n in ('include/hw/arm/guest-services/gles-names.h',
                                'contrib/guest-package/VERSION', 'contrib/export-guest-artifacts.sh',
                                'contrib/macos-app/entitlements.plist')]
    return {str(p.relative_to(root)): digest(p) for p in sorted(set(paths))}


def tree(root):
    """Hash SDK/resource contents and file inventory without embedding thousands of paths."""
    value, count = hashlib.sha256(), 0
    for path in sorted(root.rglob('*')):
        if path.is_file():
            name = str(path.relative_to(root))
            mode = path.stat().st_mode & 0o111
            value.update(f'{name}\0{digest(path)}\0{mode}\0'.encode())
            count += 1
    if not count:
        raise ValueError(f'empty build input: {root}')
    return {'path': str(root.resolve()), 'sha256': value.hexdigest(), 'files': count}


def context():
    home = Path.home() / 'Developer'
    armv6 = Path(os.environ.get('ARMV6_SDK', home / 'ipod2g-re/OldSDK/iPhoneOS3.1.3.sdk'))
    ipad = Path(os.environ.get('IPAD_SDK', home / 'qemu-ios-files/ipad1/sdk/x-iPhoneSDK3_2_2/Payload/Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS3.2.sdk'))
    tools = {name: Path(subprocess.check_output(['xcrun', '--find', name], text=True).strip())
             for name in ('clang', 'ld', 'nm')}
    for name in ('lipo', 'file', 'ldid'):
        path = shutil.which(os.environ.get('LDID', 'ldid') if name == 'ldid' else name)
        if not path:
            raise ValueError(f'guest build tool not found: {name}')
        tools[name] = Path(path)
    resource = Path(subprocess.check_output([str(tools['clang']), '-print-resource-dir'], text=True).strip())
    tools['python'] = Path(sys.executable)
    return {'sdk': {'armv6': tree(armv6), 'ipad': tree(ipad)},
            'tools': {name: {'path': str(p.resolve()), 'sha256': digest(p)} for name, p in tools.items()},
            'clang_headers': tree(resource / 'include'), 'python_version': sys.version}


if __name__ == '__main__':
    if sys.argv[1:] == ['--components']:
        print(' '.join(COMPONENTS))
    elif sys.argv[1:]:
        raise SystemExit('usage: build_inputs.py [--components]')
    else:
        root = Path(__file__).resolve().parents[2]
        print(json.dumps({'inputs': sources(root), 'build_context': context()}, sort_keys=True))
