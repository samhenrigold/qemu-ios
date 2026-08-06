#!/usr/bin/env python3
"""Edit the guest filesystem inside a NAND page image, end to end.

    editimg.py --nand <page dir> --script <shell script>

Reassembles the volume into a flat image, attaches and mounts it read-write,
runs the script with $MNT pointing at the mount point, unmounts, checks the
result with fsck_hfs, and writes the changed blocks back into the page files.
Nothing is written back if fsck is not happy.

Only the page files whose contents actually changed are touched, and each
page's 64-byte spare area is preserved.

The golden image must never be passed here - work on a copy.
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dumpvol
import packvol

GOLDEN = "/Users/shg/Developer/qemu-ios-files/nand"


def run(cmd, **kw):
    return subprocess.run(cmd, check=True, capture_output=True, text=True, **kw)


def attach(img):
    out = run(["hdiutil", "attach", "-imagekey", "diskimage-class=CRawDiskImage",
               "-nomount", img]).stdout
    return out.split()[0]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--nand", required=True, help="page directory to edit in place")
    ap.add_argument("--script", required=True, help="shell script run with $MNT set")
    ap.add_argument("--workdir", default=None)
    ap.add_argument("--blocks", type=int, default=128000)
    a = ap.parse_args()

    if os.path.realpath(a.nand) == os.path.realpath(GOLDEN):
        raise SystemExit("refusing to edit the golden image; work on a copy")

    work = a.workdir or tempfile.mkdtemp(prefix="editimg.")
    os.makedirs(work, exist_ok=True)
    img = os.path.join(work, "volume.img")
    mnt = os.path.join(work, "mnt")
    os.makedirs(mnt, exist_ok=True)

    print("[1/5] reassembling volume ...")
    sys.argv = ["dumpvol", "--nand", a.nand, "--out", img, "--blocks", str(a.blocks)]
    dumpvol.main()

    dev = attach(img)
    print("[2/5] attached %s, mounting read-write" % dev)
    ok = False
    try:
        # `diskutil mount`, NOT `mount -t hfs`. The raw mount(8) call fails on
        # recent macOS ("returned non-zero exit status 1") even as root, which
        # read as a permissions problem and is not one -- diskutil mounts the
        # very same case-sensitive HFS+ volume read-write as an ordinary user.
        # So this whole script needs no sudo; if you find yourself reaching for
        # it here, the mount is not why.
        run(["diskutil", "mount", "-mountPoint", mnt, dev])
        try:
            print("[3/5] running %s" % a.script)
            env = dict(os.environ, MNT=mnt)
            r = subprocess.run(["/bin/sh", a.script], env=env, text=True,
                               capture_output=True)
            sys.stdout.write(r.stdout)
            sys.stderr.write(r.stderr)
            if r.returncode:
                raise SystemExit("script failed with %d" % r.returncode)
            for junk in (".fseventsd", ".Spotlight-V100", ".Trashes", ".TemporaryItems"):
                shutil.rmtree(os.path.join(mnt, junk), ignore_errors=True)
        finally:
            # Match the mount above: umount(8) is the raw counterpart to the
            # mount(8) that no longer works here.
            subprocess.run(["diskutil", "unmount", mnt])
        print("[4/5] fsck_hfs ...")
        r = subprocess.run(["fsck_hfs", "-n", dev], capture_output=True, text=True)
        sys.stdout.write(r.stdout[-800:])
        if "appears to be OK" not in r.stdout:
            raise SystemExit("fsck_hfs is not happy; refusing to write back")
        ok = True
    finally:
        subprocess.run(["hdiutil", "detach", dev], capture_output=True)

    if ok:
        print("[5/5] writing changed blocks back into %s" % a.nand)
        sys.argv = ["packvol", "--img", img, "--nand", a.nand,
                    "--blocks", str(a.blocks), "--apply"]
        packvol.main()
    if not a.workdir:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
