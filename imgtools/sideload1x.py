#!/usr/bin/env python3
"""Install .app bundles into a stopped iPhone OS 1.x device (M68 iPhone, N45 iPod touch 1G).

    sideload1x.py --nand DEV/nand --overlay OVL APP.app [APP.app ...] [--remove NAME.app ...]

1.x has no installd, no code signing and no App Store: an app is a bundle in /Applications, which is how
the 2007 jailbreak community installed theirs. This writes the bundles into the system volume of the page
store the emulator serves (FirmwareKit's N45NAND layout, bank<N>/<page>.page, 2048 + 64 bytes), through
the overlay; the base is never touched. The volume is read through the legacy FTL's own context (its
logical block map and log blocks, as the guest left them; a fresh device's is the prepared one), mounted
read-write on the host, the bundles copied in, fsck_hfs checked, and each changed logical page written
over the physical page the FTL maps it to, spare kept. The FTL's context is unchanged, so on the next boot
it reads the new data where it expects the old.

The device must have been shut down cleanly (the FTL context is the last page of its context block, and
the HFS journal is empty); the tool refuses otherwise. docs/m68/sideload.md
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


MAP_PAGES = 4               # N45NAND.mapTables: the lbn -> vbn map is 4096 u16s, four context pages
LOGS = 17                   # log blocks the FTL keeps (FTLCxt.pLog; the 18th slot is unused)
VBLOCKS = 4096 - FTL_START  # virtual blocks per bank-wide superblock row (N45NAND.blocksPerBank - ftlStart)


def vlocation(vpn, banks):
    """An FTL virtual page (virtual block 0 = physical block 201, a superblock striped over the banks)."""
    sb = PPB * banks
    v = vpn + FTL_START * sb
    return v % banks, v // sb * PPB + (v // banks) % PPB


def data_spare(lpn):
    return struct.pack("<II", lpn, 0) + b"\xff\x40\xff\xff" + bytes(SPARE - 12)


class Store:
    """The page store as the guest sees it (overlay over base, blk<N>.erased markers honored), addressed by
    FTL logical page through the FTL's context. Context layout: openiBoot's s5l8900 FTLCxt, which
    FirmwareKit's N45NAND.ftlMeta writes: map pages at 0x38, log-offset pages at 0x110, the log table at
    0x1A4 (20-byte entries: usn, vbn, lbn), the context blocks at 0x312. Spare: usnDec u32 at 0, type at 9."""

    def __init__(self, base, overlay):
        self.base, self.overlay = base, overlay
        self.banks = len([d for d in os.listdir(base) if d.startswith("bank")])
        self.sb = PPB * self.banks
        self.open_ftl()

    def page(self, bank, page):
        name = "bank%d/%d.page" % (bank, page)
        for root in (self.overlay, self.base):
            try:
                with open(os.path.join(root, name), "rb") as f:
                    d = f.read()
                return d[:PAGE].ljust(PAGE, b"\0"), d[PAGE:PAGE + SPARE]
            except FileNotFoundError:
                if root == self.overlay and os.path.exists(os.path.join(root, "bank%d/blk%d.erased" % (bank, page // PPB))):
                    return None
        return None

    def vread(self, vpn):
        return self.page(*vlocation(vpn, self.banks))

    def open_ftl(self):
        # The context moves: the FTL takes new context blocks from its free pool (FTLCxt.FTLCtrlBlock, also
        # kept in the VFL context). Rather than read the VFL, look at every virtual block's first page: the
        # current context block is the context-typed one with the lowest usnDec (a ring of decreasing numbers).
        newest = None
        for vb in range(VBLOCKS):
            r = self.vread(vb * self.sb)
            if r and 0x43 <= r[1][9] <= 0x4F:
                usn = struct.unpack_from("<I", r[1], 0)[0]
                if newest is None or usn < newest[0]:
                    newest = (usn, vb)
        if newest is None:
            sys.exit("%s: no FTL context found" % self.base)
        vb = newest[1]
        last = next((r for r in (self.vread(vb * self.sb + i) for i in range(self.sb - 1, 0, -1)) if r), None)
        if not last or last[1][9] != 0x43:
            sys.exit("the FTL was not shut down cleanly (its last context page is not the context); boot the "
                     "device and power it off from the guest first")
        cxt = last[0]
        if vb not in struct.unpack_from("<3H", cxt, 0x312):
            sys.exit("FTL context in virtual block %d does not list itself as a context block" % vb)
        self.map = []
        for ptr in struct.unpack_from("<%dI" % MAP_PAGES, cxt, 0x38):
            self.map += struct.unpack("<%dH" % (PAGE // 2), self.vread(ptr)[0])
        self.logs = {}
        for i in range(LOGS):
            vbn, lbn = struct.unpack_from("<HH", cxt, 0x1A4 + 20 * i + 4)
            if vbn != 0xFFFF:
                self.logs[lbn] = (vbn, i)
        if self.logs:
            need = (LOGS * self.sb * 2 + PAGE - 1) // PAGE
            raw = b"".join(self.vread(p)[0] for p in struct.unpack_from("<%dI" % need, cxt, 0x110))
            self.offsets = struct.unpack_from("<%dH" % (LOGS * self.sb), raw)

    def vpn(self, lpn):
        lbn, off = divmod(lpn, self.sb)
        if lbn in self.logs:
            vbn, i = self.logs[lbn]
            o = self.offsets[i * self.sb + off]
            if o != 0xFFFF:
                return vbn * self.sb + o
        return self.map[lbn] * self.sb + off

    def read(self, lpn):
        return self.vread(self.vpn(lpn)) or (bytes(PAGE), None)

    def write(self, lpn, data, spare):
        bank, page = vlocation(self.vpn(lpn), self.banks)
        p = os.path.join(self.overlay, "bank%d" % bank, "%d.page" % page)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "wb") as f:
            f.write(data + (spare or data_spare(lpn)))


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


def fsck(img):
    """fsck_hfs -fn's findings on a raw image (empty when it is clean)."""
    dev = run("hdiutil", "attach", "-imagekey", "diskimage-class=CRawDiskImage", "-nomount", img).split()[0]
    try:
        r = subprocess.run(["fsck_hfs", "-fn", dev.replace("/dev/disk", "/dev/rdisk")], capture_output=True, text=True)
    finally:
        run("hdiutil", "detach", dev)
    if r.returncode == 0:
        return set()
    found = {l.strip() for l in r.stdout.splitlines()
             if l.startswith("   ") and not l.strip().startswith(("Executing fsck_hfs", "The volume name"))}
    return found or {"fsck_hfs exit %d: %s" % (r.returncode, (r.stdout + r.stderr).strip()[-300:])}


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
    st = Store(a.nand, a.overlay)
    print("FTL context: %d log blocks in use" % len(st.logs))

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
        before = fsck(img)      # what 1.x's own HFS leaves that modern fsck flags (a folder it made has no
        if before:              # HasFolderCount flag): tolerated as found, never anything new
            print("fsck_hfs findings already on the device's volume (kept as they are):\n  " + "\n  ".join(sorted(before)))
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
        new = fsck(img) - before
        if new:
            sys.exit("fsck_hfs found new damage, nothing written:\n" + "\n".join(sorted(new)))
        changed = 0
        with open(img, "rb") as f:
            for i in range(pages):
                d = f.read(PAGE)
                old, spare = st.read(FIRST_LBA + i)
                if d != old:
                    st.write(FIRST_LBA + i, d, spare)
                    changed += 1
        print("%d pages written to %s" % (changed, a.overlay))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main()
