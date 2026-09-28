#!/usr/bin/env python3
"""Full-volume manifest of an iPod NAND image, for diffing two images.

    nand_manifest.py --nand <page dir> --mnt <read-only mount of the same volume> > m.tsv

Owner/group/mode/dates come from the HFS+ catalog itself (hfsvol, read-only) because a
host mount is `noowners` and reports every file as the mounting user. Content hashes
come from the mount (sha256 of the data fork; a symlink hashes its target string).
One TSV row per catalog record:
    path  type  uid  gid  mode  size  rsrc  sha256  create  mod  attrmod
"""
import argparse
import hashlib
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../imgtools"))
import hfsvol  # noqa: E402


def records(nand):
    vol = hfsvol.Volume(nand, writable=False)
    bt = hfsvol.BTree(vol.catalog)
    for _n, buf in bt.leaf_nodes():
        for rec in bt.records(buf):
            if len(rec) < 10:
                continue
            parent, name, body = hfsvol.cat_key(rec)
            rtype = struct.unpack_from(">h", rec, body)[0]
            if rtype not in (1, 2):
                continue
            cnid, cr, md, am = struct.unpack_from(">IIII", rec, body + 8)
            uid, gid, _af, _of, mode = struct.unpack_from(">IIBBH", rec, body + 32)
            size = rsrc = 0
            if rtype == 2:
                size = struct.unpack_from(">Q", rec, body + 88)[0]
                rsrc = struct.unpack_from(">Q", rec, body + 168)[0]
            yield parent, name, cnid, rtype, uid, gid, mode, size, rsrc, cr, md, am


def sha(path):
    if os.path.islink(path):
        return "L:" + hashlib.sha256(os.readlink(path).encode()).hexdigest()
    if not os.path.isfile(path):
        return "-"
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--nand", required=True)
    ap.add_argument("--mnt", required=True)
    a = ap.parse_args()
    recs = list(records(a.nand))
    names = {r[2]: (r[0], r[1]) for r in recs}

    def path(cnid):
        parts = []
        while cnid in names and cnid != 2:
            cnid, nm = names[cnid]
            parts.append(nm)
        return "/" + "/".join(reversed(parts))

    rows = []
    for parent, name, cnid, rtype, uid, gid, mode, size, rsrc, cr, md, am in recs:
        p = path(cnid)
        h = sha(os.path.join(a.mnt, p.lstrip("/"))) if rtype == 2 else "-"
        rows.append("\t".join(map(str, (p, "f" if rtype == 2 else "d", uid, gid, "%06o" % mode,
                                        size, rsrc, h, cr, md, am))))
    sys.stdout.write("\n".join(sorted(rows)) + "\n")


if __name__ == "__main__":
    main()
