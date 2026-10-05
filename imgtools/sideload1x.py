#!/usr/bin/env python3
"""Install .app bundles into an iPhone OS 1.x device (M68 iPhone, N45 iPod touch 1G) before it boots.

    sideload1x.py --nand DEV/nand --overlay OVL APP.app [APP.app ...] [--remove NAME.app ...]

1.x has no installd, no code signing and no App Store: an app is a bundle in /Applications, which is how
the 2007 jailbreak community installed theirs. This writes the bundles into the system volume of the page
store the emulator serves (FirmwareKit's N45NAND layout, bank<N>/<page>.page, 2048 + 64 bytes), through
the overlay: the volume is assembled (overlay over base), mounted read-write on the host, the bundles
copied in, fsck_hfs checked, and only the changed pages written to OVL. The base is never touched.

The overlay must not have been booted yet: once the guest's FTL runs it moves logical pages to its log
blocks, and this tool knows only the prepared layout (logical page -> fixed physical page). An overlay
this tool alone has written (its .sideload record) is fine, so installs can be repeated before a boot.
ponytail: a booted device needs a legacy-FTL reader of the overlay; until then, sideload onto a fresh
overlay (the device's nand/ is never booted directly).
docs/m68/sideload.md
"""
import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile

PAGE, SPARE, PPB = 2048, 64, 128
FTL_START, DATA_START, FIRST_LBA = 201, 23, 3      # N45NAND.swift: ftlStart, dataStart, firstLBA


def location(lpn, banks):
    sb = PPB * banks
    v = DATA_START * sb + lpn + FTL_START * sb
    return v % banks, v // sb * PPB + (v // banks) % PPB


def data_spare(lpn):
    return struct.pack("<II", lpn, 0) + b"\xff\x40\xff\xff" + bytes(SPARE - 12)


class Store:
    def __init__(self, base, overlay):
        self.base, self.overlay = base, overlay
        self.banks = len([d for d in os.listdir(base) if d.startswith("bank")])

    def path(self, root, lpn):
        b, p = location(lpn, self.banks)
        return os.path.join(root, "bank%d" % b, "%d.page" % p)

    def read(self, lpn):
        for root in (self.overlay, self.base):
            try:
                with open(self.path(root, lpn), "rb") as f:
                    d = f.read()
                return d[:PAGE].ljust(PAGE, b"\0"), d[PAGE:PAGE + SPARE]
            except FileNotFoundError:
                pass
        return bytes(PAGE), None

    def write(self, lpn, data, spare):
        p = self.path(self.overlay, lpn)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "wb") as f:
            f.write(data + (spare or data_spare(lpn)))
        return os.path.relpath(p, self.overlay)


def check_overlay(overlay):
    record = os.path.join(overlay, ".sideload")
    ours = set(open(record).read().split()) if os.path.exists(record) else set()
    for d, _, files in os.walk(overlay):
        for f in files:
            rel = os.path.relpath(os.path.join(d, f), overlay)
            if rel != ".sideload" and rel not in ours:
                sys.exit("%s: the guest has written this overlay (%s); sideload before the first boot "
                         "(see the ponytail note in this file)" % (overlay, rel))
    return ours


def journal_snapshot(img):
    """The journal info block and journal as the device left them, to put back after the host's mount.
    macOS rewrites the journal header for 512-byte blocks; 1.x adopts the header's block size and then fails
    I/O on its 2048-byte pages (mkdir/bind EINVAL all over userland). FirmwareKit's VolumeMount does the same."""
    with open(img, "rb") as f:
        f.seek(1024)
        vh = f.read(512)
        if not struct.unpack_from(">I", vh, 4)[0] & 0x2000:          # kHFSVolumeJournaledBit
            return []
        block = struct.unpack_from(">I", vh, 40)[0]
        jib_at = struct.unpack_from(">I", vh, 12)[0] * block
        f.seek(jib_at)
        jib = f.read(block)
        off, size = struct.unpack_from(">QQ", jib, 36)
        f.seek(off)
        hdr = f.read(32)
        if not struct.unpack_from(">I", jib, 0)[0] & 4 and hdr[8:16] != hdr[16:24]:
            sys.exit("%s: the volume's journal is not empty (start != end); boot and shut down cleanly first" % img)
        f.seek(off)
        return [(jib_at, jib), (off, f.read(size))]


def run(*cmd):
    return subprocess.run(cmd, check=True, capture_output=True, text=True).stdout


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--nand", required=True, help="the device's read-only page store (DEV/nand)")
    ap.add_argument("--overlay", required=True, help="the overlay the emulator will boot with")
    ap.add_argument("--remove", action="append", default=[], help="an /Applications bundle name to delete")
    ap.add_argument("apps", nargs="*")
    a = ap.parse_args()
    for app in a.apps:
        if not (app.rstrip("/").endswith(".app") and os.path.isfile(os.path.join(app, "Info.plist"))):
            sys.exit("%s: not an .app bundle with an Info.plist" % app)
    os.makedirs(a.overlay, exist_ok=True)
    ours = check_overlay(a.overlay)
    st = Store(a.nand, a.overlay)

    hdr, _ = st.read(FIRST_LBA)
    if hdr[1024:1026] not in (b"H+", b"HX"):
        sys.exit("%s: no HFS+ volume at logical page %d" % (a.nand, FIRST_LBA))
    block_size, total = struct.unpack_from(">II", hdr, 1024 + 40)
    pages = block_size * total // PAGE

    tmp = tempfile.mkdtemp(prefix="sideload1x.")
    img, mnt = os.path.join(tmp, "system.img"), os.path.join(tmp, "mnt")
    try:
        with open(img, "wb") as f:
            for i in range(pages):
                f.write(st.read(FIRST_LBA + i)[0])
        journal = journal_snapshot(img)
        os.mkdir(mnt)
        out = run("hdiutil", "attach", "-imagekey", "diskimage-class=CRawDiskImage", "-nobrowse", "-owners", "off",
                  "-mountpoint", mnt, img)
        dev = out.split()[0]
        try:
            apps_dir = os.path.join(mnt, "Applications")
            for name in a.remove:
                shutil.rmtree(os.path.join(apps_dir, os.path.basename(name)), ignore_errors=True)
            for app in a.apps:
                dst = os.path.join(apps_dir, os.path.basename(app.rstrip("/")))
                shutil.rmtree(dst, ignore_errors=True)
                run("ditto", "--norsrc", "--noextattr", app, dst)
                exe = os.path.join(dst, __import__("plistlib").load(open(os.path.join(dst, "Info.plist"), "rb"))
                                   ["CFBundleExecutable"])
                os.chmod(exe, 0o755)
                print("installed /Applications/%s" % os.path.basename(dst))
            run("dot_clean", "-m", apps_dir)
            for junk in (".fseventsd", ".Spotlight-V100", ".Trashes", ".TemporaryItems", ".DS_Store"):
                p = os.path.join(mnt, junk)
                shutil.rmtree(p, ignore_errors=True) if os.path.isdir(p) else (os.path.exists(p) and os.remove(p))
        finally:
            run("hdiutil", "detach", dev)
        with open(img, "r+b") as f:     # the device's own journal back: see journal_snapshot
            for at, data in journal:
                f.seek(at)
                f.write(data)
        dev = run("hdiutil", "attach", "-imagekey", "diskimage-class=CRawDiskImage", "-nomount", img).split()[0]
        try:
            fsck = subprocess.run(["fsck_hfs", "-fn", dev.replace("/dev/disk", "/dev/rdisk")], capture_output=True,
                                  text=True)
        finally:
            run("hdiutil", "detach", dev)
        if fsck.returncode != 0:
            sys.exit("fsck_hfs failed, nothing written:\n" + fsck.stdout + fsck.stderr)
        changed = 0
        with open(img, "rb") as f:
            for i in range(pages):
                d = f.read(PAGE)
                old, spare = st.read(FIRST_LBA + i)
                if d != old:
                    ours.add(st.write(FIRST_LBA + i, d, spare))
                    changed += 1
        with open(os.path.join(a.overlay, ".sideload"), "w") as f:
            f.write("\n".join(sorted(ours)) + "\n")
        print("%d pages written to %s" % (changed, a.overlay))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main()
