#!/usr/bin/env python3
"""Write a flat HFS+ disk image back into the NAND page-file layout.

The inverse of dumpvol.py. Together they make the guest filesystem editable
with ordinary tools: dump -> attach read-write -> change files -> detach ->
pack. macOS's own HFS+ implementation does the B-tree and bitmap work, which is
both far less code and far more trustworthy than reimplementing it.

Only blocks that actually differ are rewritten, and each page's 64-byte spare
area is preserved.
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nandblob as nb
from ftlmap import predict

BLOCK = 4096
BLANK_SPARE = b"\x00" * 8 + b"\xff\x00\xff\x00" + b"\x00" * 52


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--img", required=True)
    ap.add_argument("--nand", required=True, help="page directory to update in place")
    ap.add_argument("--blocks", type=int, default=128000)
    ap.add_argument("--apply", action="store_true")
    a = ap.parse_args()

    changed = created = same = 0
    with open(a.img, "rb") as img:
        for n in range(a.blocks):
            new = img.read(BLOCK)
            if len(new) < BLOCK:
                new = new.ljust(BLOCK, b"\x00")
            cs, pg = predict(n)
            path = nb.page_path(a.nand, cs, pg)
            spare = BLANK_SPARE
            try:
                with open(path, "rb") as f:
                    old = f.read()
                if len(old) >= nb.PAGE:
                    spare = old[nb.DATA : nb.PAGE]
                if old[:BLOCK] == new:
                    same += 1
                    continue
                changed += 1
            except FileNotFoundError:
                if new == b"\x00" * BLOCK:
                    same += 1
                    continue
                created += 1
            if not a.apply:
                continue
            os.makedirs(os.path.dirname(path), exist_ok=True)
            tmp = path + ".tmp"
            with open(tmp, "wb") as f:
                f.write(new)
                f.write(spare)
            os.replace(tmp, path)

    print("%d unchanged, %d rewritten, %d newly created%s"
          % (same, changed, created, "" if a.apply else "  (dry run)"))


if __name__ == "__main__":
    main()
