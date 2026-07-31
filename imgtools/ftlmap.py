#!/usr/bin/env python3
"""Sample the HFS+ allocation-block -> NAND (cs, page) mapping.

Adding a file to the image (as opposed to editing one in place) means writing
to blocks that are currently free - and a free block has no content to search
for, so its physical page has to be *predicted*. This samples the mapping using
files whose contents can be located by search and whose catalog record gives the
allocation block, then reports how well a candidate formula fits.

Ground-truth file contents come from the separately mountable rootfs; only the
block numbers are read out of the NAND image.
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nandblob as nb
import hfsfile as hf


def sample(nand, blob, rootfs, limit, minsize=256, maxsize=4096):
    hf.register(blob, nand)
    idx = nb.load_index(blob)
    import mmap

    f = open(blob, "rb")
    mm = mmap.mmap(f.fileno(), 0, prot=mmap.PROT_READ)

    pairs, skipped = [], {"nocontent": 0, "ambig": 0, "norec": 0, "multirec": 0}
    for dirpath, _dirs, files in os.walk(rootfs):
        for name in files:
            if len(pairs) >= limit:
                break
            p = os.path.join(dirpath, name)
            try:
                st = os.lstat(p)
            except OSError:
                continue
            if not os.path.isfile(p) or os.path.islink(p):
                continue
            if not (minsize <= st.st_size <= maxsize):
                continue
            try:
                with open(p, "rb") as fh:
                    content = fh.read(min(st.st_size, 256))
            except OSError:
                continue

            # unique page whose data area *starts* with this content
            hits, pos = [], 0
            while True:
                q = mm.find(content, pos)
                if q < 0 or len(hits) > 2:
                    break
                if q % nb.DATA == 0:
                    hits.append(q)
                pos = q + 1
            if not hits:
                skipped["nocontent"] += 1
                continue
            if len(hits) > 1:
                skipped["ambig"] += 1
                continue
            cs, pg, _o = nb.locate(idx, hits[0])

            recs = [r for r in hf.find_catalog_records(blob, name)
                    if r["logical_size"] == st.st_size and r["total_blocks"] == 1]
            if not recs:
                skipped["norec"] += 1
                continue
            if len(recs) > 1:
                skipped["multirec"] += 1
                continue
            pairs.append((recs[0]["extents"][0][0], cs, pg, name))
        if len(pairs) >= limit:
            break
    mm.close()
    f.close()
    return pairs, skipped


def predict(n):
    """HFS+ allocation block -> physical (cs, page).

    Four consecutive logical blocks form a row striped across the four chip
    selects; rows alternate between two 128-page erase blocks (the two planes),
    so the in-erase-block offset only advances every second row. Erase blocks
    are consumed in pairs, two per group of 256 rows, starting at erase block 2.
    """
    r, cs = divmod(n + 3, 4)
    eb = 2 * (r // 256) + 2 + (r % 2)
    return cs, eb * 128 + (r % 256) // 2


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--nand", required=True)
    ap.add_argument("--blob", required=True)
    ap.add_argument("--rootfs", default="/Volumes/SugarBowl5F138.N72OS")
    ap.add_argument("--limit", type=int, default=60)
    a = ap.parse_args()

    pairs, skipped = sample(a.nand, a.blob, a.rootfs, a.limit)
    print("samples: %d   skipped: %s" % (len(pairs), skipped))
    exact = 0
    for blk, cs, pg, name in sorted(pairs):
        pcs, ppg = predict(blk)
        ok = (pcs == cs and ppg == pg)
        exact += ok
        print("block %-7d -> cs%d page %-6d  predicted cs%d page %-6d  %s  %s"
              % (blk, cs, pg, pcs, ppg, "OK " if ok else "d=%+d" % (pg - ppg)
                 if pcs == cs else "CS!", name))
    print("\nformula matched %d/%d" % (exact, len(pairs)))


if __name__ == "__main__":
    main()
