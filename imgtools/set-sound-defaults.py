#!/usr/bin/env python3
"""Enable the five iPod touch Sounds switches in a mounted default image.

The keys and Calendar Alerts string value come from 7E18 Preferences.app's
Sounds.plist. Run through editimg.py, or as part of bake-guest-tools.sh.
Existing devices keep their own preferences; this defines new-device defaults.
"""
import argparse
import os
from pathlib import Path
import plistlib

DEFAULTS = {
    'com.apple.mobilemail.plist': {
        'PlayNewMailSound': True,
        'PlaySentMailSound': True,
    },
    'com.apple.springboard.plist': {
        'calendar-alarm': '/Applications/MobileCal.app/alarm.aiff',
        'lock-unlock': True,
    },
    'com.apple.preferences.sounds.plist': {'keyboard': True},
}


def enable_sounds(root):
    preferences = Path(root) / 'private/var/mobile/Library/Preferences'
    preferences.mkdir(parents=True, exist_ok=True)
    for name, changes in DEFAULTS.items():
        path = preferences / name
        raw = path.read_bytes() if path.exists() else b''
        values = plistlib.loads(raw) if raw else {}
        if not isinstance(values, dict):
            raise ValueError(f'{path}: expected a preferences dictionary')
        values.update(changes)
        fmt = plistlib.FMT_BINARY if raw.startswith(b'bplist00') else plistlib.FMT_XML
        updated = plistlib.dumps(values, fmt=fmt, sort_keys=False)
        if updated != raw:
            path.write_bytes(updated)
        print(f'{name}: enabled {", ".join(changes)}')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', default=os.environ.get('MNT'))
    args = parser.parse_args()
    if not args.root:
        parser.error('--root or MNT is required')
    enable_sounds(args.root)
