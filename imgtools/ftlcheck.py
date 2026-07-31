#!/usr/bin/env python3
"""Validate the block -> (cs, page) formula against whole multi-block files.

Far stronger than spot-checking single-block files: for each file it takes the
extents out of the NAND's own catalog record, maps every allocation block
through the formula, and requires the page at that address to hold exactly the
matching 4096-byte chunk of the file's contents (read from the separately
mountable rootfs). A wrong formula cannot survive hundreds of consecutive
blocks.
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nandblob as nb
import hfsfile as hf
from ftlmap import predict


def check_file(nand, path, rec):
    with open(path, "rb") as f:
        content = f.read()
    blocks = []
    for start, count in rec["extents"]:
        blocks.extend(range(start, start + count))
    ok = bad = missing = 0
    for i, blk in enumerate(blocks):
        chunk = content[i * nb.DATA : (i + 1) * nb.DATA]
        if not chunk:
            break
        cs, pg = predict(blk)
        p = nb.page_path(nand, cs, pg)
        if not os.path.exists(p):
            missing += 1
            continue
        with open(p, "rb") as f:
            data = f.read(nb.DATA)
        if data[: len(chunk)] == chunk:
            ok += 1
        else:
            bad += 1
    return ok, bad, missing


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--nand", required=True)
    ap.add_argument("--blob", required=True)
    ap.add_argument("--rootfs", default="/Volumes/SugarBowl5F138.N72OS")
    ap.add_argument("--limit", type=int, default=40)
    ap.add_argument("--min-size", type=int, default=200000)
    a = ap.parse_args()
    hf.register(a.blob, a.nand)

    tot_ok = tot_bad = tot_missing = nfiles = 0
    for dirpath, _dirs, files in os.walk(a.rootfs):
        for name in files:
            if nfiles >= a.limit:
                break
            p = os.path.join(dirpath, name)
            if os.path.islink(p) or not os.path.isfile(p):
                continue
            try:
                size = os.path.getsize(p)
            except OSError:
                continue
            if size < a.min_size:
                continue
            recs = [r for r in hf.find_catalog_records(a.blob, name)
                    if r["logical_size"] == size]
            if len(recs) != 1:
                continue
            ok, bad, missing = check_file(a.nand, p, recs[0])
            nfiles += 1
            tot_ok += ok
            tot_bad += bad
            tot_missing += missing
            flag = "OK  " if bad == 0 else "FAIL"
            print("%s %-42s %8d bytes  %4d blocks ok, %d bad, %d missing"
                  % (flag, name[:42], size, ok, bad, missing))
        if nfiles >= a.limit:
            break
    print("\n%d files: %d blocks correct, %d wrong, %d page file absent"
          % (nfiles, tot_ok, tot_bad, tot_missing))


if __name__ == "__main__":
    main()
