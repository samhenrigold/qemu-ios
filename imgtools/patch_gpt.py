#!/usr/bin/env python3
"""Resize the single GPT partition of a NAND page image.

The synthetic image carries a real GPT in its first three logical blocks:
logical 0 is empty (no protective MBR), logical 1 is the EFI PART header and
logical 2 is a one-entry partition array. Logical n lives at the (cs, page) the
layout formula gives for volume block n - 3, so those are cs0/cs1/cs2 page 256.

Growing the HFS+ volume is not enough on its own -- disk0s1 is created from the
partition entry, so its ending LBA has to move with the volume, and both CRC32s
(the entry array's, and the header's own) have to be recomputed or the guest
sees a corrupt GPT.
"""

import argparse
import binascii
import os
import struct
import sys

sys.path.insert(0, "/Users/shg/Developer/qemu-ios/.claude/worktrees/consolidate/imgtools")
from ftlmap import predict

DATA = 4096


def page_of_logical(nand, logical):
    cs, pg = predict(logical - 3)
    return os.path.join(nand, "cs%d" % cs, "%d.page" % pg)


def rw(path):
    with open(path, "rb") as f:
        buf = bytearray(f.read())
    return buf


def flush(path, buf):
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(buf)
    os.replace(tmp, path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--nand", required=True)
    ap.add_argument("--blocks", type=int, required=True,
                    help="new HFS+ volume size in 4096-byte allocation blocks")
    ap.add_argument("--slack", type=int, default=11,
                    help="LBAs the partition holds beyond the volume (the "
                         "stock image has 11)")
    ap.add_argument("--apply", action="store_true")
    a = ap.parse_args()

    hpath = page_of_logical(a.nand, 1)
    epath = page_of_logical(a.nand, 2)
    hdr, ent = rw(hpath), rw(epath)

    if bytes(hdr[:8]) != b"EFI PART":
        raise SystemExit("no GPT header at %s" % hpath)
    hdrsz = struct.unpack_from("<I", hdr, 0x0C)[0]
    nent, entsz = struct.unpack_from("<II", hdr, 0x50)

    def hdr_crc(b):
        t = bytearray(b[:hdrsz])
        struct.pack_into("<I", t, 0x10, 0)
        return binascii.crc32(bytes(t)) & 0xFFFFFFFF

    if hdr_crc(hdr) != struct.unpack_from("<I", hdr, 0x10)[0]:
        raise SystemExit("existing header CRC is wrong; refusing to guess")

    start, old_end = struct.unpack_from("<QQ", ent, 0x20)
    new_end = start + a.blocks - 1 + a.slack
    print("partition %d..%d -> %d..%d  (%d LBAs for %d volume blocks)"
          % (start, old_end, start, new_end, new_end - start + 1, a.blocks))

    struct.pack_into("<Q", ent, 0x28, new_end)
    struct.pack_into("<I", hdr, 0x58,
                     binascii.crc32(bytes(ent[: nent * entsz])) & 0xFFFFFFFF)
    struct.pack_into("<I", hdr, 0x10, hdr_crc(hdr))
    print("entry-array CRC %08x, header CRC %08x"
          % (struct.unpack_from("<I", hdr, 0x58)[0],
             struct.unpack_from("<I", hdr, 0x10)[0]))

    if not a.apply:
        print("(dry run)")
        return
    flush(hpath, hdr)
    flush(epath, ent)
    print("wrote %s and %s" % (hpath, epath))


if __name__ == "__main__":
    main()
