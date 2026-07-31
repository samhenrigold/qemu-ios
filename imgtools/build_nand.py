#!/usr/bin/env python3
"""Build a synthetic NAND page image from an IPSW root filesystem.

    build_nand.py --rootfs <rootfs.dmg|rootfs.img> [--key <hex>]
                  --kernelcache <kernelcache.release.s5l8720x>
                  --out <new page directory>

`nand=` is not a dump of real flash (see the long comment in
hw/arm/ipod_touch_fmss.c): it is a single HFSX volume laid across four
chip-selects by the closed-form formula in ftlmap.py, plus a handful of
FTL/VFL bookkeeping pages and a GPT that the formula does not cover. This
script is the generator that was missing from the tree.

What it does, in order:

  1. decrypts the IPSW root filesystem DMG (optional; xpwn's `dmg` tool) into a
     flat raw image. Apple ships that DMG as a *bare* HFSX volume - no
     partition map - with 4096-byte allocation blocks, which is exactly the
     shape dumpvol.py/packvol.py expect.
  2. grows the volume to --blocks allocation blocks with `hdiutil resize`, so
     it fills the region the layout formula addresses and the guest has free
     space to write into.
  3. mounts it read-write and applies the build edits:
       - /private/etc/fstab is rewritten to a single read-write root, because
         the synthetic image is one volume with no separate /private/var
         partition (see --no-fstab to keep the stock two-partition fstab),
       - the IPSW kernelcache is copied, still encrypted and unmodified, to
         /System/Library/Caches/com.apple.kernelcaches/kernelcache.s5l8720x,
         the path iBoot asks FMSS for. The emulated AES engine decrypts it
         in-guest, so nothing here needs a key.
       - an optional extra --script, run with $MNT, for anything else.
  4. checks the result with `fsck_hfs -n` and refuses to continue if unhappy.
  5. rewrites the ownership of the files it created. macOS will not let a
     non-root user chown inside the image, so files created by step 3 land as
     uid/gid 99 ("unknown"); the catalog records are patched offline instead.
  6. writes the page directory: every page the formula covers comes from the
     volume, and every page it does not (the GPT at device LBA 0-2, the FTL's
     own metadata pages, the NAND driver signature) is copied verbatim from
     --template, since that bookkeeping describes the flash, not the
     filesystem.

The result is a directory in the same shape as the existing images:
`cs{0..3}/<page>.page`, 4096 data bytes + 64 spare bytes each.
"""

import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hfsvol as H
import nandblob as nb
from ftlmap import predict

BLOCK = 4096
SECTOR = 512
BLANK_SPARE = b"\x00" * 8 + b"\xff\x00\xff\x00" + b"\x00" * 52

GOLDEN = "/Users/shg/Developer/qemu-ios-files/nand"
DEFAULT_TEMPLATE = "/Users/shg/Developer/qemu-ios-files/nand-canonical"
DEFAULT_DMGTOOL = "/Users/shg/Downloads/OldSDK/dmg"

KC_DIR = "System/Library/Caches/com.apple.kernelcaches"
KC_NAME = "kernelcache.s5l8720x"
FSTAB = "private/etc/fstab"
FSTAB_RW = "/dev/disk0s1 / hfs rw 0 1\n"
JUNK = (".fseventsd", ".Spotlight-V100", ".Trashes", ".TemporaryItems", ".DS_Store")


def run(cmd, **kw):
    return subprocess.run(cmd, check=True, capture_output=True, text=True, **kw)


# --- offline catalog surgery ---------------------------------------------

class FlatVolume(H.Volume):
    """hfsvol.Volume backed by a flat image file instead of the page files."""

    def __init__(self, path, writable=False):
        self.f = open(path, "r+b" if writable else "rb")
        self.writable = writable
        self.dirty = set()
        self._cache = {}
        self.vh = self.read_block(0)[1024:1536]
        sig = struct.unpack_from(">2s", self.vh, 0)[0]
        if sig not in (b"H+", b"HX"):
            raise RuntimeError("no HFS+ volume header (got %r)" % sig)
        self.signature = sig
        self.block_size = struct.unpack_from(">I", self.vh, 40)[0]
        self.total_blocks = struct.unpack_from(">I", self.vh, 44)[0]
        self.free_blocks = struct.unpack_from(">I", self.vh, 48)[0]
        self.file_count = struct.unpack_from(">I", self.vh, 32)[0]
        self.folder_count = struct.unpack_from(">I", self.vh, 36)[0]
        if self.block_size != BLOCK:
            raise RuntimeError("unexpected allocation block size %d" % self.block_size)
        self.catalog = H.Fork(self, self.vh, 272)

    def read_block(self, n):
        if n in self._cache:
            return self._cache[n]
        self.f.seek(n * BLOCK)
        d = bytearray(self.f.read(BLOCK).ljust(BLOCK, b"\x00"))
        self._cache[n] = d
        return d

    def write_block(self, n, data):
        if not self.writable:
            raise RuntimeError("volume opened read-only")
        self._cache[n] = bytearray(data)
        self.dirty.add(n)

    def flush(self):
        for n in sorted(self.dirty):
            self.f.seek(n * BLOCK)
            self.f.write(bytes(self._cache[n]))
        self.f.flush()
        n = len(self.dirty)
        self.dirty.clear()
        return n

    def close(self):
        self.f.close()


def set_owner(img, paths, uid=0, gid=0):
    """Force uid/gid on the given volume-relative paths, in the catalog itself.

    HFSPlusCatalogFile/Folder both carry HFSPlusBSDInfo 32 bytes into the
    record, so this is an in-place edit of two big-endian words - no B-tree
    restructuring, no size change.
    """
    vol = FlatVolume(img, writable=True)
    bt = H.BTree(vol.catalog)
    want = {tuple(p.strip("/").split("/")) for p in paths}
    # (parentCNID, name) -> cnid, for every record, so paths can be resolved
    byname = {}
    nodes = []
    for n, buf in bt.leaf_nodes():
        nodes.append((n, bytearray(buf)))
        for rec in bt.records(buf):
            if len(rec) < 10:
                continue
            parent, name, body = H.cat_key(rec)
            if len(rec) < body + 8:
                continue
            rt = struct.unpack_from(">H", rec, body)[0]
            if rt in (H.kHFSPlusFolderRecord, H.kHFSPlusFileRecord):
                cnid = struct.unpack_from(">I", rec, body + 8)[0]
                byname[(parent, name)] = cnid

    targets = set()
    for comps in want:
        cnid = 2  # kHFSRootFolderID
        for c in comps:
            key = (cnid, c)
            if key not in byname:
                cnid = None
                break
            cnid = byname[key]
        if cnid is None:
            raise RuntimeError("path not found in catalog: %s" % "/".join(comps))
        targets.add(cnid)

    changed = 0
    for n, buf in nodes:
        touched = False
        _f, _b, _k, _h, num_recs, offs = bt.node_info(bytes(buf))
        for i in range(num_recs):
            start, end = offs[i], offs[i + 1]
            rec = bytes(buf[start:end])
            if len(rec) < 10:
                continue
            parent, name, body = H.cat_key(rec)
            if len(rec) < body + 48:
                continue
            rt = struct.unpack_from(">H", rec, body)[0]
            if rt not in (H.kHFSPlusFolderRecord, H.kHFSPlusFileRecord):
                continue
            cnid = struct.unpack_from(">I", rec, body + 8)[0]
            if cnid not in targets:
                continue
            o = start + body + 32
            cur = struct.unpack_from(">II", buf, o)
            if cur == (uid, gid):
                continue
            struct.pack_into(">II", buf, o, uid, gid)
            touched = True
            changed += 1
        if touched:
            vol.catalog.write(n * bt.node_size, bytes(buf))
    vol.flush()
    vol.close()
    return changed


# --- page image writing ---------------------------------------------------

def formula_pages(blocks):
    used = {cs: set() for cs in range(4)}
    for n in range(blocks):
        cs, pg = predict(n)
        used[cs].add(pg)
    return used


def write_pages(img, out, template, blocks):
    """Write the page directory: volume blocks by formula, the rest verbatim."""
    used = formula_pages(blocks)

    copied = 0
    for cs in range(4):
        src = os.path.join(template, "cs%d" % cs)
        dst = os.path.join(out, "cs%d" % cs)
        os.makedirs(dst, exist_ok=True)
        if not os.path.isdir(src):
            continue
        for name in os.listdir(src):
            if not name.endswith(".page"):
                continue
            pg = int(name[:-5])
            if pg in used[cs]:
                continue          # filesystem data; comes from the volume
            shutil.copyfile(os.path.join(src, name), os.path.join(dst, name))
            copied += 1

    written = 0
    with open(img, "rb") as f:
        for n in range(blocks):
            data = f.read(BLOCK)
            if len(data) < BLOCK:
                data = data.ljust(BLOCK, b"\x00")
            cs, pg = predict(n)
            path = nb.page_path(out, cs, pg)
            tmp = path + ".tmp"
            with open(tmp, "wb") as g:
                g.write(data)
                g.write(BLANK_SPARE)
            os.replace(tmp, path)
            written += 1
    return written, copied


# --- the build ------------------------------------------------------------

def decrypt_rootfs(dmg, key, tool, out):
    print("[1/7] decrypting %s" % os.path.basename(dmg))
    run([tool, "extract", dmg, out, "-k", key])


def resize(img, blocks):
    sectors = blocks * BLOCK // SECTOR
    # hdiutil grows the backing file itself and moves the alternate volume
    # header/bitmap with it; pre-truncating makes it think there is nothing
    # to do and leaves the filesystem at its original size.
    run(["hdiutil", "resize", "-sectors", str(sectors),
         "-imagekey", "diskimage-class=CRawDiskImage", img])
    if os.path.getsize(img) != blocks * BLOCK:
        raise SystemExit("resize left the image at %d bytes, wanted %d"
                         % (os.path.getsize(img), blocks * BLOCK))


def attach(img):
    out = run(["hdiutil", "attach", "-imagekey", "diskimage-class=CRawDiskImage",
               "-nomount", img]).stdout
    return out.split()[0]


def volume_info(img):
    v = FlatVolume(img)
    info = (v.signature.decode(), v.block_size, v.total_blocks, v.free_blocks,
            v.file_count, v.folder_count)
    v.close()
    return info


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rootfs", required=True,
                    help="IPSW root filesystem: raw HFS image, or .dmg with --key")
    ap.add_argument("--key", default=None, help="rootfs DMG key (no IV form)")
    ap.add_argument("--dmgtool", default=DEFAULT_DMGTOOL)
    ap.add_argument("--kernelcache", default=None,
                    help="IPSW kernelcache img3, copied in unmodified/encrypted")
    ap.add_argument("--template", default=DEFAULT_TEMPLATE,
                    help="page directory to take the GPT and FTL metadata pages from")
    ap.add_argument("--out", required=True, help="page directory to create")
    ap.add_argument("--blocks", type=int, default=128000)
    ap.add_argument("--script", default=None, help="extra shell script, run with $MNT")
    ap.add_argument("--no-fstab", action="store_true",
                    help="keep the stock two-partition read-only-root fstab")
    ap.add_argument("--workdir", default=None)
    ap.add_argument("--keep-work", action="store_true")
    a = ap.parse_args()

    if os.path.exists(a.out):
        raise SystemExit("%s already exists; refusing to overwrite an image" % a.out)
    if os.path.realpath(os.path.dirname(a.out) or ".") == os.path.realpath(GOLDEN):
        raise SystemExit("refusing to write inside the golden image")
    if not os.path.isdir(a.template):
        raise SystemExit("no template page directory at %s" % a.template)

    work = a.workdir or tempfile.mkdtemp(prefix="build_nand.")
    os.makedirs(work, exist_ok=True)
    img = os.path.join(work, "volume.img")
    mnt = os.path.join(work, "mnt")
    os.makedirs(mnt, exist_ok=True)

    if a.key:
        decrypt_rootfs(a.rootfs, a.key, a.dmgtool, img)
    else:
        print("[1/7] copying %s" % os.path.basename(a.rootfs))
        shutil.copyfile(a.rootfs, img)

    print("[2/7] growing the volume to %d x %d-byte blocks" % (a.blocks, BLOCK))
    resize(img, a.blocks)
    sig, bs, total, free, files, dirs = volume_info(img)
    print("      %s blocksize=%d total=%d free=%d files=%d dirs=%d"
          % (sig, bs, total, free, files, dirs))
    if total != a.blocks:
        raise SystemExit("resize produced %d blocks, wanted %d" % (total, a.blocks))

    created = []
    dev = attach(img)
    ok = False
    try:
        print("[3/7] attached %s, mounting read-write" % dev)
        run(["mount", "-t", "hfs", "-o", "noowners,nobrowse", dev, mnt])
        try:
            if not a.no_fstab:
                print("      rewriting /etc/fstab for a single read-write root")
                with open(os.path.join(mnt, FSTAB), "w") as f:
                    f.write(FSTAB_RW)
            if a.kernelcache:
                dst = os.path.join(mnt, KC_DIR, KC_NAME)
                print("      installing %s" % os.path.join("/", KC_DIR, KC_NAME))
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                # plain byte copy: shutil.copyfile() goes through fcopyfile()
                # on macOS and drags the source's xattrs (com.apple.quarantine
                # on a downloaded IPSW) into the guest filesystem.
                with open(a.kernelcache, "rb") as s, open(dst, "wb") as d:
                    shutil.copyfileobj(s, d)
                created.append(os.path.join(KC_DIR, KC_NAME))
            if a.script:
                print("      running %s" % a.script)
                r = subprocess.run(["/bin/sh", a.script],
                                   env=dict(os.environ, MNT=mnt),
                                   text=True, capture_output=True)
                sys.stdout.write(r.stdout)
                sys.stderr.write(r.stderr)
                if r.returncode:
                    raise SystemExit("script failed with %d" % r.returncode)
            for junk in JUNK:
                p = os.path.join(mnt, junk)
                shutil.rmtree(p, ignore_errors=True)
                if os.path.isfile(p):
                    os.unlink(p)
        finally:
            subprocess.run(["umount", mnt])
        print("[4/7] fsck_hfs -n ...")
        r = subprocess.run(["fsck_hfs", "-n", dev], capture_output=True, text=True)
        sys.stdout.write(r.stdout[-600:])
        if "appears to be OK" not in r.stdout:
            raise SystemExit("fsck_hfs is not happy; refusing to build the image")
        ok = True
    finally:
        subprocess.run(["hdiutil", "detach", dev], capture_output=True)
    if not ok:
        raise SystemExit("build aborted")

    if created:
        print("[5/7] setting uid/gid 0 on %d created path(s)" % len(created))
        print("      patched %d catalog record(s)" % set_owner(img, created))
    else:
        print("[5/7] nothing to re-own")

    print("[6/7] writing page files into %s" % a.out)
    written, copied = write_pages(img, a.out, a.template, a.blocks)
    print("      %d filesystem pages, %d metadata pages copied from %s"
          % (written, copied, os.path.basename(a.template)))

    print("[7/7] done. volume: %s" % img if a.keep_work else "[7/7] done.")
    if not a.keep_work and not a.workdir:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
