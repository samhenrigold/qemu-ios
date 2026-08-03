#!/usr/bin/env python3
"""Grow the guest volume of a NAND page image, in place, on a copy.

    grow_volume.py --nand <existing page dir> --out <new page dir> --gb 2

The emulated flash is far bigger than the volume on it. The FMSS model reports
chip id 0xb614d5ad on all four chip-selects; whimory's part table (in the 7E18
kernelcache at file offset 0x5842a1) gives that part 0x1080 = 4224 blocks per
chip-select, so the device the guest's FTL opens is

    4 CS x 4096 usable blocks x 128 pages x 4096 bytes = 8 GiB

and ftlmap.predict() -- the layout formula -- is exactly the interleave that
fills erase blocks 2..4094 of that device. The stock image only *populates* the
first ~253 erase blocks, because the HFS+ volume on it is 128000 blocks (500 MB)
and the GPT partition entry says so. Both of those are software.

So growing the device is three edits and no new flash:

  1. dump the volume flat, `hdiutil resize` it to the new block count,
  2. move the GPT partition entry's ending LBA and recompute both CRC32s
     (the entry array's, at header +0x58, and the header's own, at +0x10),
  3. pack the flat image back into page files.

Step 3 costs almost nothing: the grown region is free space, so it is all
zeros, and packvol.py skips an all-zero block that has no page file -- which
reads back from the emulator as an erased page, byte for byte the same thing.
Growing 500 MB -> 7 GiB adds *one* page file (the alternate volume header).

NOTE: this needs the matching device-model change. fmss_generated_layout()
bounds writes by NAND_GENERATED_TOTAL_BLOCKS, hardcoded to the old 128000, and
silently drops any page whose logical block is past it -- which is precisely the
space this tool adds. See the diff alongside this file.
"""

import argparse
import binascii
import os
import shutil
import struct
import subprocess
import sys
import tempfile

IMGTOOLS = "/Users/shg/Developer/qemu-ios/imgtools"
sys.path.insert(0, IMGTOOLS)
from ftlmap import predict

BLOCK = 4096
SECTOR = 512
# eb = 2*(r//256)+2+(r%2) must stay clear of erase block 4095, which holds
# NANDDRIVERSIGN and DEVICEINFOBBT on every chip-select.
MAX_BLOCKS = 2_090_000


def page_of_logical(nand, logical):
    cs, pg = predict(logical - 3)
    return os.path.join(nand, "cs%d" % cs, "%d.page" % pg)


def run(cmd):
    return subprocess.run(cmd, check=True, capture_output=True, text=True)


def volume_header(path):
    with open(path, "rb") as f:
        vh = f.read(2048)[1024:1536]
    return (vh[:2], struct.unpack_from(">I", vh, 40)[0],
            struct.unpack_from(">I", vh, 44)[0],
            struct.unpack_from(">I", vh, 48)[0])


def patch_gpt(nand, blocks, slack=11):
    hpath, epath = page_of_logical(nand, 1), page_of_logical(nand, 2)
    with open(hpath, "rb") as f:
        hdr = bytearray(f.read())
    with open(epath, "rb") as f:
        ent = bytearray(f.read())
    if bytes(hdr[:8]) != b"EFI PART":
        raise SystemExit("no GPT header at %s" % hpath)
    hdrsz = struct.unpack_from("<I", hdr, 0x0C)[0]
    nent, entsz = struct.unpack_from("<II", hdr, 0x50)

    def hcrc(b):
        t = bytearray(b[:hdrsz])
        struct.pack_into("<I", t, 0x10, 0)
        return binascii.crc32(bytes(t)) & 0xFFFFFFFF

    if hcrc(hdr) != struct.unpack_from("<I", hdr, 0x10)[0]:
        raise SystemExit("existing GPT header CRC is wrong; refusing to guess")

    start = struct.unpack_from("<Q", ent, 0x20)[0]
    struct.pack_into("<Q", ent, 0x28, start + blocks - 1 + slack)
    struct.pack_into("<I", hdr, 0x58,
                     binascii.crc32(bytes(ent[: nent * entsz])) & 0xFFFFFFFF)
    struct.pack_into("<I", hdr, 0x10, hcrc(hdr))
    for p, b in ((hpath, hdr), (epath, ent)):
        with open(p + ".tmp", "wb") as f:
            f.write(b)
        os.replace(p + ".tmp", p)
    return start, start + blocks - 1 + slack


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--nand", required=True, help="source page directory")
    ap.add_argument("--out", required=True, help="page directory to create")
    ap.add_argument("--blocks", type=int, help="new size in 4096-byte blocks")
    ap.add_argument("--gb", type=float, help="new size in GiB (alternative)")
    ap.add_argument("--old-blocks", type=int, default=128000)
    a = ap.parse_args()

    blocks = a.blocks or int(a.gb * (1 << 30) // BLOCK)
    if not blocks:
        raise SystemExit("give --blocks or --gb")
    if blocks <= a.old_blocks:
        raise SystemExit("new size must be larger than the current volume")
    if blocks > MAX_BLOCKS:
        raise SystemExit("%d blocks runs into erase block 4095 (max %d)"
                         % (blocks, MAX_BLOCKS))
    if os.path.exists(a.out):
        raise SystemExit("%s already exists" % a.out)

    work = tempfile.mkdtemp(prefix="grow_volume.")
    img = os.path.join(work, "volume.img")
    try:
        print("[1/5] dumping %d blocks" % a.old_blocks)
        run([sys.executable, os.path.join(IMGTOOLS, "dumpvol.py"),
             "--nand", a.nand, "--out", img, "--blocks", str(a.old_blocks)])

        print("[2/5] resizing to %d blocks (%.2f GiB)"
              % (blocks, blocks * BLOCK / (1 << 30)))
        run(["hdiutil", "resize", "-sectors", str(blocks * BLOCK // SECTOR),
             "-imagekey", "diskimage-class=CRawDiskImage", img])
        sig, bs, total, free = volume_header(img)
        print("      %s blocksize=%d total=%d free=%d (%.2f GiB free)"
              % (sig.decode(), bs, total, free, free * BLOCK / (1 << 30)))
        if total != blocks:
            raise SystemExit("resize produced %d blocks" % total)

        print("[3/5] fsck_hfs -n")
        dev = run(["hdiutil", "attach", "-imagekey",
                   "diskimage-class=CRawDiskImage", "-nomount",
                   img]).stdout.split()[0]
        try:
            r = subprocess.run(["fsck_hfs", "-n", dev],
                               capture_output=True, text=True)
            sys.stdout.write(r.stdout[-300:])
            if "appears to be OK" not in r.stdout:
                raise SystemExit("fsck_hfs is not happy; aborting")
        finally:
            subprocess.run(["hdiutil", "detach", dev], capture_output=True)

        print("[4/5] cloning %s -> %s" % (a.nand, a.out))
        # -c is APFS clonefile: the copy shares blocks until something changes.
        run(["cp", "-Rc", a.nand, a.out])
        s, e = patch_gpt(a.out, blocks)
        print("      GPT partition now LBA %d..%d" % (s, e))

        print("[5/5] packing")
        r = run([sys.executable, os.path.join(IMGTOOLS, "packvol.py"),
                 "--img", img, "--nand", a.out, "--blocks", str(blocks),
                 "--apply"])
        sys.stdout.write(r.stdout)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print("done: %s" % a.out)


if __name__ == "__main__":
    main()
