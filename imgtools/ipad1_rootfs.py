#!/usr/bin/env python3
"""Userland images for the ipad1 machine: a pristine 7B500 system volume plus a seeded data volume.

    ipad1_rootfs.py build [--rootfs rootfs.dmg] [--mbr rdisk0-head4M.bin] [--out DIR]
                          [--data-size 1g] [--lockdown DIR|none] [--disable LABEL]... [--ro-root]
    ipad1_rootfs.py fetch-lockdown [DIR]     copy /var/root/Library/Lockdown off the real iPad (ssh)
    ipad1_rootfs.py --selfcheck

`build` writes DIR/system.img and DIR/data.img, which go straight into

    ipad1_nand.py build --mbr rdisk0-head4M.bin --system DIR/system.img --data DIR/data.img --out NAND

Why not the captured hw2/rdisk0s1-system.img: that volume is the jailbroken unit's, with /Applications
symlinked into /var/stash and a Cydia-era daemon set. The IPSW rootfs is what a restore lays down, and
its catalog already carries Apple's uid/gid/modes, so nothing needs re-owning on the system side.

system.img  = the IPSW's Apple_HFSX slice, grown to partition 1's size (327680 x 4 KiB, from the MBR),
              with three edits made through a mount: /etc/fstab normalised to
              "/dev/disk0s1 / rw" + "/dev/disk0s2 /private/var" (the emulator's data partition is plain
              0xAF at disk0s2, see ipad1_nand.py; rw root is insurance for a failed data mount),
              SpringBoard's launchd job gets CA_ENABLE_OGL=0 / MBX2D_PAGE_FLIP=0 (userland-gl-display.md
              §1.3/§1.5: without them SpringBoard re-runs the whole EAGL/GLEngine probe every frame),
              and any --disable label gets Disabled=true. No Mach-O is touched, so the boot needs no
              code-signing boot-args.
data.img    = a fresh journaled HFSX "Data" volume seeded the way mobile_obliterator seeds it: a copy of
              the system volume's own /private/var skeleton. /var/mobile and /var/ea become 501:501,
              everything else 0:0, matching the real iPad. With --lockdown, that directory (the real
              unit's activation record, device keys and pair records) lands in /var/root/Library/Lockdown.
"""
import argparse
import os
import plistlib
import shutil
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build_nand as bn                      # resize(), set_owner(), volume_info(), run(), JUNK
from ipad1_nand import make_hfs_image, mbr_parts, parse_size

FILES = os.path.expanduser("~/Developer/qemu-ios-files/ipad1")
IPAD_SSH = os.path.join(FILES, "ipad-ssh.sh")
BLOCK = 4096
FSTAB = "/dev/disk0s1 / hfs rw 0 1\n/dev/disk0s2 /private/var hfs rw,nosuid,nodev 0 2\n"
FSTAB_RO = FSTAB.replace("/ hfs rw", "/ hfs ro", 1)
SB_JOB = "System/Library/LaunchDaemons/com.apple.SpringBoard.plist"
SB_ENV = {"CA_ENABLE_OGL": "0", "MBX2D_PAGE_FLIP": "0"}
MOBILE_TOP = ("mobile", "ea")                # uid 501 on the real unit; everything else under /var is root


# --- pure helpers (covered by --selfcheck) ------------------------------------------------------

def apm_hfs_slice(raw):
    """(byte offset, byte length) of the HFS partition in an Apple Partition Map."""
    assert raw[:2] == b"ER", "not an Apple partition map"
    bs = struct.unpack_from(">H", raw, 2)[0]
    n = struct.unpack_from(">I", raw, bs + 4)[0]
    for i in range(1, n + 1):
        e = raw[bs * i:bs * (i + 1)]
        start, count = struct.unpack_from(">II", e, 8)
        if e[48:80].split(b"\0")[0] in (b"Apple_HFSX", b"Apple_HFS"):
            return start * bs, count * bs
    raise ValueError("no Apple_HFS(X) partition")


def edit_plist(data, fn):
    """Apply fn(dict) to a plist, keeping its binary/XML flavour."""
    d = plistlib.loads(data)
    fn(d)
    return plistlib.dumps(d, fmt=plistlib.FMT_BINARY if data.startswith(b"bplist") else plistlib.FMT_XML)


def rewrite_plist(path, fn):
    """Read fully before opening for write: open(path, "wb") truncates the file first."""
    with open(path, "rb") as f:
        new = edit_plist(f.read(), fn)
    with open(path, "wb") as f:      # in place, so the catalog record (and Apple's uid 0) survives
        f.write(new)


def springboard_env(d):
    assert d.get("Label") == "com.apple.SpringBoard"
    d.setdefault("EnvironmentVariables", {}).update(SB_ENV)
    # /dev/console is crw--w--w- on the real unit, so the mobile user can append: with serial=3 this
    # puts SpringBoard's own stderr on the serial log next to the kernel's.
    d["StandardOutPath"] = d["StandardErrorPath"] = "/dev/console"


def owner_for(relpath):
    return (501, 501) if relpath.split("/", 1)[0] in MOBILE_TOP else (0, 0)


def hfs_info(img):
    """(signature, allocation block size, total blocks, free blocks); the IPSW volume uses 8 KiB blocks."""
    with open(img, "rb") as f:
        f.seek(1024)
        vh = f.read(512)
    sig = vh[:2]
    if sig not in (b"H+", b"HX"):
        raise SystemExit("%s: no HFS+ volume header (got %r)" % (img, sig))
    return (sig.decode(),) + struct.unpack_from(">III", vh, 40)


# --- host plumbing ------------------------------------------------------------------------------

def extract_rootfs(src, out):
    """Raw HFS volume from an IPSW rootfs DMG (UDIF+APM), or a plain copy if src already is one."""
    with open(src, "rb") as f:
        f.seek(1024)
        if f.read(2) in (b"H+", b"HX"):
            print("      %s is already a bare HFS volume; copying" % os.path.basename(src))
            shutil.copyfile(src, out)
            return
    work = tempfile.mkdtemp(prefix="ipad1_rootfs.")
    try:
        bn.run(["hdiutil", "convert", src, "-format", "UDTO", "-o", os.path.join(work, "raw")])
        with open(os.path.join(work, "raw.cdr"), "rb") as f, open(out, "wb") as o:
            off, ln = apm_hfs_slice(f.read(64 * 512))
            f.seek(off)
            while ln:
                chunk = f.read(min(ln, 1 << 24))
                o.write(chunk)
                ln -= len(chunk)
    finally:
        shutil.rmtree(work, ignore_errors=True)


class Mounted:
    """attach a raw HFS image and mount it read-write at `mnt` (diskutil, no sudo: see editimg.py)."""

    def __init__(self, img, mnt):
        self.img, self.mnt, self.ok = img, mnt, False

    def __enter__(self):
        os.makedirs(self.mnt, exist_ok=True)
        self.dev = bn.attach(self.img)
        bn.run(["diskutil", "mount", "-mountPoint", self.mnt, self.dev])
        return self

    def __exit__(self, et, *_):
        for junk in bn.JUNK:
            p = os.path.join(self.mnt, junk)
            shutil.rmtree(p, ignore_errors=True)
        subprocess.run(["diskutil", "unmount", self.dev], capture_output=True)
        try:
            if et is None:
                r = subprocess.run(["fsck_hfs", "-fn", self.dev], capture_output=True, text=True)  # -f: journaled data volume
                self.ok = "appears to be OK" in r.stdout
                if not self.ok:
                    sys.stdout.write(r.stdout[-600:])
        finally:
            subprocess.run(["hdiutil", "detach", self.dev], capture_output=True)
        if et is None and not self.ok:
            raise SystemExit("fsck_hfs is not happy with %s" % self.img)


def build(a):
    os.makedirs(a.out, exist_ok=True)
    system, data = os.path.join(a.out, "system.img"), os.path.join(a.out, "data.img")
    p1 = mbr_parts(open(a.mbr, "rb").read(512))[0]
    if p1[0] != 0xAF:
        raise SystemExit("%s: partition 1 is type %#x, not Apple_HFS" % (a.mbr, p1[0]))

    print("[1/4] system volume from %s" % a.rootfs)
    extract_rootfs(a.rootfs, system)
    sig, bs, total, free = hfs_info(system)
    print("      %s %d x %d B blocks, %d free; growing to %d MiB (partition 1)" % (sig, total, bs, free, p1[2] * BLOCK >> 20))
    if total * bs > p1[2] * BLOCK:
        raise SystemExit("volume already larger than partition 1")
    # hdiutil grows the backing file itself (see build_nand.resize) but with 8 KiB blocks it stops one
    # 4 KiB sector short of the partition. Pad the file and move the alternate volume header to the
    # new end - 1024, where fsck_hfs and the kernel look for it; the stale copy is harmless.
    bn.run(["hdiutil", "resize", "-sectors", str(p1[2] * BLOCK // 512), "-imagekey", "diskimage-class=CRawDiskImage", system])
    old, new = os.path.getsize(system), p1[2] * BLOCK
    if old > new:
        raise SystemExit("resize overshot partition 1")
    with open(system, "r+b") as f:
        f.seek(old - 1024)
        avh = f.read(512)
        f.truncate(new)
        f.seek(new - 1024)
        f.write(avh)

    print("[2/4] editing the system volume")
    skeleton, mobile_paths, all_paths = tempfile.mkdtemp(prefix="ipad1_var."), [], []
    with Mounted(system, os.path.join(a.out, "mnt-system")) as m:
        with open(os.path.join(m.mnt, "private/etc/fstab"), "w") as f:
            f.write(FSTAB_RO if a.ro_root else FSTAB)
        rewrite_plist(os.path.join(m.mnt, SB_JOB), springboard_env)
        for label in a.disable:
            rewrite_plist(os.path.join(m.mnt, "System/Library/LaunchDaemons", label + ".plist"),
                          lambda d: d.__setitem__("Disabled", True))
        # /private/var skeleton for the data volume, taken while the volume is mounted
        shutil.rmtree(skeleton)
        shutil.copytree(os.path.join(m.mnt, "private/var"), skeleton, symlinks=True)
    print("      fstab %s root, SpringBoard env %s%s" % ("ro" if a.ro_root else "rw", SB_ENV,
          ", disabled %s" % a.disable if a.disable else ""))

    print("[3/4] data volume (%s) seeded from /private/var%s" % (a.data_size,
          " + " + a.lockdown if a.lockdown else ""))
    if a.lockdown:
        shutil.copytree(a.lockdown, os.path.join(skeleton, "root/Library/Lockdown"), dirs_exist_ok=True)
    os.replace(make_hfs_image(data + ".dmg", parse_size(a.data_size)), data)
    with Mounted(data, os.path.join(a.out, "mnt-data")) as m:
        shutil.copytree(skeleton, m.mnt, symlinks=True, dirs_exist_ok=True)
        for root, dnames, fnames in os.walk(m.mnt):
            dnames[:] = [d for d in dnames if d not in bn.JUNK]   # macOS droppings, removed at unmount
            for n in dnames + [f for f in fnames if f not in bn.JUNK]:
                rel = os.path.relpath(os.path.join(root, n), m.mnt)
                (mobile_paths if owner_for(rel) == (501, 501) else all_paths).append(rel)
    shutil.rmtree(skeleton, ignore_errors=True)
    # the mount is noowners as an ordinary user, so everything landed uid 99/501: fix the catalog offline
    n0 = bn.set_owner(data, all_paths, 0, 0)
    n1 = bn.set_owner(data, mobile_paths, 501, 501)
    print("      %d paths -> 0:0, %d paths -> 501:501 (%d catalog records patched)" % (len(all_paths), len(mobile_paths), n0 + n1))
    for d in ("mnt-system", "mnt-data"):
        shutil.rmtree(os.path.join(a.out, d), ignore_errors=True)

    print("[4/4] done:\n    %s/ipad1_nand.py build --mbr %s --system %s --data %s --out %s/nand-userland"
          % (os.path.dirname(os.path.abspath(__file__)), a.mbr, system, data, a.out))


def fetch_lockdown(out):
    """Pull /var/root/Library/Lockdown (activation record, device keys, pair records) off the real iPad."""
    os.makedirs(out, exist_ok=True)
    subprocess.run("%s 'tar cf - -C /var/root/Library Lockdown' | tar xf - -C %s --strip-components 1"
                   % (IPAD_SSH, out), shell=True, check=True)
    print("fetched %s: %s" % (out, sorted(os.listdir(out))))


def selfcheck():
    apm = bytearray(512 * 4)
    apm[:2] = b"ER"
    struct.pack_into(">H", apm, 2, 512)
    for i, (start, count, typ) in enumerate([(1, 63, b"Apple_partition_map"), (64, 1000, b"Apple_HFSX")], 1):
        e = 512 * i
        apm[e:e + 2] = b"PM"
        struct.pack_into(">II", apm, e + 4, 2, start)
        struct.pack_into(">I", apm, e + 12, count)
        apm[e + 48:e + 48 + len(typ)] = typ
    assert apm_hfs_slice(bytes(apm)) == (64 * 512, 1000 * 512)

    job = {"Label": "com.apple.SpringBoard", "EnvironmentVariables": {"X": "1"}, "KeepAlive": True}
    out = edit_plist(plistlib.dumps(job, fmt=plistlib.FMT_BINARY), springboard_env)
    sb = plistlib.loads(out)
    assert out.startswith(b"bplist") and sb["EnvironmentVariables"] == {"X": "1", **SB_ENV}
    assert sb["StandardErrorPath"] == "/dev/console" and sb["KeepAlive"] is True
    out = edit_plist(plistlib.dumps({"Label": "x"}), lambda d: d.__setitem__("Disabled", True))
    assert out.startswith(b"<?xml") and plistlib.loads(out)["Disabled"] is True
    try:
        edit_plist(plistlib.dumps({"Label": "other"}), springboard_env)
        assert False
    except AssertionError:
        pass

    assert owner_for("mobile") == owner_for("mobile/Library/Preferences/a.plist") == owner_for("ea") == (501, 501)
    assert owner_for("root/Library/Lockdown") == owner_for("mobileX") == owner_for("db") == (0, 0)
    assert FSTAB_RO.splitlines()[0] == "/dev/disk0s1 / hfs ro 0 1" and "s2s1" not in FSTAB


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--selfcheck", action="store_true")
    sub = ap.add_subparsers(dest="cmd")
    b = sub.add_parser("build")
    b.add_argument("--rootfs", default=os.path.join(FILES, "7B500/dec/rootfs.dmg"), help="decrypted IPSW rootfs DMG, or a bare HFS image")
    b.add_argument("--mbr", default=os.path.join(FILES, "hw2/rdisk0-head4M.bin"))
    b.add_argument("--out", default=os.path.join(FILES, "userland"))
    b.add_argument("--data-size", default="1g")
    b.add_argument("--lockdown", default=os.path.join(FILES, "hw2/lockdown"), help="fetch-lockdown output; 'none' to skip")
    b.add_argument("--disable", action="append", default=[], metavar="LABEL", help="launchd job to mark Disabled")
    b.add_argument("--ro-root", action="store_true", help="keep the stock read-only root")
    f = sub.add_parser("fetch-lockdown")
    f.add_argument("dir", nargs="?", default=os.path.join(FILES, "hw2/lockdown"))
    a = ap.parse_args()
    selfcheck()
    if a.cmd == "build":
        if a.lockdown == "none" or not os.path.isdir(a.lockdown):
            print("      no lockdown seed at %s (run fetch-lockdown); activation state will be Unactivated" % a.lockdown)
            a.lockdown = None
        build(a)
    elif a.cmd == "fetch-lockdown":
        fetch_lockdown(a.dir)
    elif not a.selfcheck:
        ap.print_help()


if __name__ == "__main__":
    main()
