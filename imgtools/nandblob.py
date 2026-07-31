#!/usr/bin/env python3
"""Build/search a flat blob of the raw NAND page image.

The image is a directory of `cs{0..3}/<pagenr>.page` files, each 4160 bytes
(4096 data + 64 spare).  Concatenating the data areas in a fixed order gives a
single file we can mmap and search quickly, plus an index that maps any blob
offset back to (cs, pagenr, offset-in-page). Searching one flat file also beats
opening 128,060 small ones for every query.

Order is cs-major, page-minor: all of cs0 sorted by page number, then cs1, ...
"""

import argparse
import mmap
import os
import struct
import sys

PAGE = 4160
DATA = 4096
SPARE = 64


def page_files(nand):
    """[(cs, pagenr, path)] in blob order."""
    out = []
    for cs in range(4):
        d = os.path.join(nand, "cs%d" % cs)
        if not os.path.isdir(d):
            continue
        nums = sorted(
            int(n[:-5]) for n in os.listdir(d) if n.endswith(".page")
        )
        for n in nums:
            out.append((cs, n, os.path.join(d, "%d.page" % n)))
    return out


def build(nand, blob, spareblob=None):
    files = page_files(nand)
    idx = []
    with open(blob, "wb") as bf:
        sf = open(spareblob, "wb") if spareblob else None
        for cs, n, path in files:
            with open(path, "rb") as f:
                buf = f.read()
            if len(buf) != PAGE:
                sys.stderr.write("short page %s (%d)\n" % (path, len(buf)))
                buf = buf.ljust(PAGE, b"\x00")
            bf.write(buf[:DATA])
            if sf:
                sf.write(buf[DATA:PAGE])
            idx.append((cs, n))
        if sf:
            sf.close()
    with open(blob + ".idx", "wb") as f:
        f.write(b"NIDX0001")
        f.write(struct.pack("<I", len(idx)))
        for cs, n in idx:
            f.write(struct.pack("<BI", cs, n))
    return len(idx)


def load_index(blob):
    with open(blob + ".idx", "rb") as f:
        magic = f.read(8)
        assert magic == b"NIDX0001", magic
        (cnt,) = struct.unpack("<I", f.read(4))
        raw = f.read(cnt * 5)
    return [struct.unpack_from("<BI", raw, i * 5) for i in range(cnt)]


def locate(idx, off):
    """blob offset -> (cs, pagenr, off_in_page)"""
    i, o = divmod(off, DATA)
    cs, n = idx[i]
    return cs, n, o


def page_path(nand, cs, n):
    return os.path.join(nand, "cs%d" % cs, "%d.page" % n)


def search(blob, needle, limit=50):
    idx = load_index(blob)
    hits = []
    with open(blob, "rb") as f:
        mm = mmap.mmap(f.fileno(), 0, prot=mmap.PROT_READ)
        pos = 0
        while len(hits) < limit:
            p = mm.find(needle, pos)
            if p < 0:
                break
            hits.append((p,) + locate(idx, p))
            pos = p + 1
        mm.close()
    return hits


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--nand", default="/Users/shg/Developer/qemu-ios-files/nand")
    ap.add_argument("--blob", default="/Users/shg/Developer/qemu-ios-files/nand.blob")
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build")
    b.add_argument("--spare", default=None)
    s = sub.add_parser("search")
    s.add_argument("needle")
    s.add_argument("--hex", action="store_true")
    s.add_argument("--limit", type=int, default=50)
    a = ap.parse_args()

    if a.cmd == "build":
        n = build(a.nand, a.blob, a.spare)
        print("built %s: %d pages, %d bytes" % (a.blob, n, n * DATA))
    else:
        needle = bytes.fromhex(a.needle) if a.hex else a.needle.encode()
        for off, cs, pg, o in search(a.blob, needle, a.limit):
            print("blob=0x%x cs%d page %d +0x%x" % (off, cs, pg, o))


if __name__ == "__main__":
    main()
