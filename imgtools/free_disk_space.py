#!/usr/bin/env python3
"""Historical 2.1.1 500 MB image pruning. Do not use for current 3.1.3 image preparation.

Free enough space on the guest volume that iOS reports a non-zero "Available".

Settings > General > About shows `AmountDataAvailable` from lockdownd's
`com.apple.disk_usage` domain, and lockdownd computes it as

    AmountDataAvailable = max(0, statfs("/private/var").f_bfree * f_bsize
                                 - 167772160)          # a hard 160 MiB reserve

(`calculate_disk_usage`, lockdownd VA 0x62e0; the constant is the literal
0x0A000000 at 0x65bc).  Our 500 MB single-partition image only ever has ~56 MB
free, which is below the reserve, so About honestly reports 0 bytes.  The only
way to make it report a real number is to have more than 160 MiB free.

This deletes bulk content an English-only emulated device does not need.  It
runs against a *copy* of a NAND page directory, never the golden image.

    free_disk_space.py --nand <page dir> [--fonts] [--textinput] [--lproj]

With no selection flags all three are applied (~191 MB, leaving ~250 MB free
and About showing roughly 82 MB available).
"""

import argparse
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))

CJK_FONTS = [
    "System/Library/Fonts/Cache/STHeiti-Light.ttc",
    "System/Library/Fonts/Cache/STHeiti-Medium.ttc",
    "System/Library/Fonts/Cache/HiraginoKakuGothicProNW3.otf",
    "System/Library/Fonts/Cache/HiraginoKakuGothicProNW6.otf",
    "System/Library/Fonts/Cache/AppleGothicRegular.ttf",
]

KEEP_LPROJ = ("English.lproj", "en.lproj", "en_GB.lproj", "en_US.lproj")

SCRIPT = r'''#!/bin/bash
set -eu
cd "$MNT"
%s
sync
'''


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--nand", required=True, help="page directory (a copy!)")
    ap.add_argument("--fonts", action="store_true",
                    help="drop the five CJK fonts (~76 MB)")
    ap.add_argument("--textinput", action="store_true",
                    help="drop non-English TextInput bundles (~78 MB)")
    ap.add_argument("--lproj", action="store_true",
                    help="drop non-English .lproj directories (~37 MB)")
    a = ap.parse_args()

    if not (a.fonts or a.textinput or a.lproj):
        a.fonts = a.textinput = a.lproj = True

    steps = []
    if a.fonts:
        steps += ['rm -f "%s"' % p for p in CJK_FONTS]
    if a.textinput:
        steps.append(
            'find System/Library/TextInput -maxdepth 1 -name "TextInput_*.bundle" '
            '! -name "TextInput_en.bundle" -exec rm -rf {} +')
    if a.lproj:
        keep = " ".join('! -name "%s"' % k for k in KEEP_LPROJ)
        steps.append('find . -type d -name "*.lproj" %s -exec rm -rf {} +' % keep)

    with tempfile.NamedTemporaryFile("w", suffix=".sh", delete=False) as f:
        f.write(SCRIPT % "\n".join(steps))
        script = f.name
    os.chmod(script, 0o755)
    print("\n".join(steps))
    subprocess.run([sys.executable, os.path.join(HERE, "editimg.py"),
                    "--nand", a.nand, "--script", script], check=True)
    os.unlink(script)


if __name__ == "__main__":
    sys.exit(main())
