#!/usr/bin/env python3
"""Add EnvironmentVariables to a launchd plist stored inside the NAND image.

Usage:
    patch_launchd_env.py --nand <dir> --plist com.apple.SpringBoard.plist \
        --set CA_AUTO_ENABLE_OGL=0 --set LK_AUTO_ENABLE_OGL=0 [--apply]

Without --apply it only reports what it would do.

How it works: the catalog record for the plist is found by searching the raw
pages for its UTF-16BE name; the data page is found by searching for the first
bytes of the plist that record describes. Both are rewritten in place - the new
plist still fits in the file's single 4096-byte allocation block, so only
`dataFork.logicalSize` has to change alongside it.
"""

import argparse
import os
import plistlib
import pprint
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nandblob as nb
import hfsfile as hf


def read_file_bytes(nand, blob, name):
    """Find `name`'s catalog record and the page holding its (single-block) data."""
    hf.register(blob, nand)
    recs = hf.find_catalog_records(blob, name)
    if len(recs) != 1:
        raise SystemExit(
            "expected exactly 1 catalog record for %r, found %d: %s"
            % (name, len(recs), pprint.pformat(recs))
        )
    rec = recs[0]
    if rec["total_blocks"] != 1 or len(rec["extents"]) != 1:
        raise SystemExit(
            "file spans %d blocks / %d extents; only single-block files are "
            "supported by this tool" % (rec["total_blocks"], len(rec["extents"]))
        )
    return rec


def find_data_page(nand, blob, size, label):
    """Locate the page holding a `size`-byte bplist whose Label is `label`.

    Size alone is not enough - unrelated plists collide on length (a 751-byte
    Settings strings file sits right next to the 751-byte SpringBoard job).
    """
    hits = nb.search(blob, b"bplist00", limit=4000)
    out = []
    for _off, cs, pg, o in hits:
        if o != 0:
            continue
        with open(nb.page_path(nand, cs, pg), "rb") as f:
            page = f.read(nb.DATA)
        if plist_length(page) != size:
            continue
        try:
            if plistlib.loads(page[:size]).get("Label") != label:
                continue
        except Exception:
            continue
        out.append((cs, pg, page))
    return out


def plist_length(page):
    """Length of the bplist starting at page[0], or None."""
    import struct

    for L in range(40, len(page) + 1):
        t = page[L - 32 : L]
        if len(t) != 32:
            continue
        unused, _sv, ois, ors, nobj, _top, oto = struct.unpack(">5sBBBQQQ", t)
        if unused != b"\x00" * 5 or ois not in (1, 2, 4, 8) or ors not in (1, 2, 4, 8):
            continue
        if oto + nobj * ois + 32 != L:
            continue
        try:
            plistlib.loads(page[:L])
        except Exception:
            continue
        return L
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--nand", required=True)
    ap.add_argument("--blob", default=None)
    ap.add_argument("--plist", required=True, help="file name, e.g. com.apple.SpringBoard.plist")
    ap.add_argument("--set", action="append", default=[], metavar="KEY=VALUE")
    ap.add_argument("--apply", action="store_true")
    a = ap.parse_args()

    blob = a.blob or os.path.join(
        os.path.dirname(a.nand.rstrip("/")), os.path.basename(a.nand.rstrip("/")) + ".blob"
    )
    if not os.path.exists(blob) or os.path.getmtime(blob) < newest_page_mtime(a.nand):
        print("building blob %s ..." % blob)
        nb.build(a.nand, blob)

    rec = read_file_bytes(a.nand, blob, a.plist)
    print("catalog record: cs%d page %d rec_off 0x%x fileID %d mode 0%o size %d block %d"
          % (rec["cs"], rec["page"], rec["rec_off"], rec["file_id"], rec["mode"],
             rec["logical_size"], rec["extents"][0][0]))

    label = a.plist[:-6] if a.plist.endswith(".plist") else a.plist
    cands = find_data_page(a.nand, blob, rec["logical_size"], label)
    if len(cands) != 1:
        raise SystemExit("expected 1 data page holding a %d-byte bplist with Label %r, found %d: %s"
                         % (rec["logical_size"], label, len(cands),
                            [(c, p) for c, p, _ in cands]))
    cs, pg, page = cands[0]
    old = page[: rec["logical_size"]]
    plist = plistlib.loads(old)
    print("data page: cs%d page %d" % (cs, pg))
    print("--- current plist:")
    pprint.pprint(plist)

    env = dict(plist.get("EnvironmentVariables") or {})
    for kv in a.set:
        k, _, v = kv.partition("=")
        env[k] = v
    plist["EnvironmentVariables"] = env
    new = plistlib.dumps(plist, fmt=plistlib.FMT_BINARY)
    assert plistlib.loads(new) == plist, "round-trip failed"

    print("--- new plist (%d bytes, was %d):" % (len(new), len(old)))
    pprint.pprint(plist)
    if len(new) > nb.DATA:
        raise SystemExit("new plist exceeds one 4096-byte allocation block")

    if not a.apply:
        print("\n(dry run - pass --apply to write)")
        return

    hf.write_page_data(a.nand, cs, pg, new)
    hf.set_logical_size(a.nand, rec["cs"], rec["page"], rec["rec_off"], len(new))
    print("\nwrote data page cs%d/%d.page and logicalSize %d -> %d in cs%d/%d.page"
          % (cs, pg, rec["logical_size"], len(new), rec["cs"], rec["page"]))
    try:
        os.remove(blob)
        os.remove(blob + ".idx")
    except OSError:
        pass
    print("stale blob removed; it will be rebuilt on next run")


def newest_page_mtime(nand):
    m = 0
    for cs in range(4):
        d = os.path.join(nand, "cs%d" % cs)
        if os.path.isdir(d):
            m = max(m, os.path.getmtime(d))
    return m


if __name__ == "__main__":
    main()
