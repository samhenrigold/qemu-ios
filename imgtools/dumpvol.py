#!/usr/bin/env python3
"""Reassemble the NAND page image into a flat HFS+ disk image.

Independent verification: if the block -> page formula is right, the result is a
filesystem that `fsck_hfs -n` passes and `hdiutil attach` mounts. Blocks whose
page file is absent were never written and dump as zeros, which is what the
emulator serves for them too.
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nandblob as nb
from ftlmap import predict

BLOCK = 4096


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--nand", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--blocks", type=int, default=128000)
    a = ap.parse_args()

    zero = b"\x00" * BLOCK
    missing = 0
    with open(a.out, "wb") as out:
        for n in range(a.blocks):
            cs, pg = predict(n)
            try:
                with open(nb.page_path(a.nand, cs, pg), "rb") as f:
                    data = f.read(BLOCK)
                if len(data) < BLOCK:
                    data = data.ljust(BLOCK, b"\x00")
            except FileNotFoundError:
                data = zero
                missing += 1
            out.write(data)
    print("wrote %s (%d blocks, %d had no page file)"
          % (a.out, a.blocks, missing))


if __name__ == "__main__":
    main()
