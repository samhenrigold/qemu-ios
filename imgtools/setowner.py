#!/usr/bin/env python3
"""Set the on-disk owner, group and mode of files inside a NAND page image.

    setowner.py --nand <page dir> /System/Library/LaunchDaemons/x.plist:0:0:644

Why this exists, and why `chown` in an editimg.py script cannot do it:

`editimg.py` mounts the reassembled volume with `-o noowners`, and it does so
as an ordinary user. Two things follow, and both of them lie to you:

  * `chown` fails outright -- "Operation not permitted" -- because setting an
    owner other than your own needs root, and nothing here runs as root.
  * `ls -ln` on that mount reports EVERY file as 501:20, Apple's own included,
    because that is what an ignore-ownership mount shows. So a host-side
    listing cannot tell you what the guest will see either way.

Anything created through that mount therefore lands owned by uid 501, and
launchd on the guest **silently ignores a LaunchDaemon plist that is not owned
by root** -- no log line, no error, the daemon simply never starts. That is
exactly how the pasteboard bridge came to be "installed" in an image and yet
completely dead.

This tool sidesteps the mount entirely: it edits the HFS+ catalog B-tree in the
page files directly, patching the HFSPlusBSDInfo of the named records. Run it
AFTER editimg.py has packed its changes back.
"""

import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hfsvol

# HFSPlusBSDInfo sits at the same offset in both the file and the folder
# record: recordType(2) flags(2) valence-or-reserved(4) id(4) + five 4-byte
# dates = 32 bytes of header.
BSD_OFF = 32
ROOT_CID = 2


def index_catalog(bt):
    """(parent, name) -> (node number, record index, record offset, length)."""
    out = {}
    for num, buf in bt.leaf_nodes():
        _f, _b, _k, _h, nrecs, offs = bt.node_info(buf)
        for i in range(nrecs):
            start, end = offs[i], offs[i + 1]
            rec = buf[start:end]
            if len(rec) < 10:
                continue
            parent, name, body = hfsvol.cat_key(rec)
            if body + BSD_OFF + 16 > len(rec):
                continue
            rtype = struct.unpack_from(">h", rec, body)[0]
            if rtype not in (hfsvol.kHFSPlusFolderRecord,
                             hfsvol.kHFSPlusFileRecord):
                continue
            cnid = struct.unpack_from(">I", rec, body + 8)[0]
            out[(parent, name)] = (num, start + body, rtype, cnid)
    return out


def resolve(index, path):
    cid = ROOT_CID
    hit = None
    for part in [p for p in path.split("/") if p]:
        hit = index.get((cid, part))
        if hit is None:
            raise SystemExit("not in the catalog: %s (stuck at %r)" % (path, part))
        cid = hit[3]
    if hit is None:
        raise SystemExit("refusing to touch the volume root")
    return hit


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--nand", required=True)
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("specs", nargs="+",
                    help="PATH:uid:gid[:octal-mode] (mode replaces the "
                         "permission bits only, the file type is kept)")
    a = ap.parse_args()

    vol = hfsvol.Volume(a.nand, writable=not a.dry_run)
    bt = hfsvol.BTree(vol.catalog)
    index = index_catalog(bt)

    for spec in a.specs:
        parts = spec.split(":")
        if len(parts) not in (3, 4):
            raise SystemExit("bad spec %r" % spec)
        path, uid, gid = parts[0], int(parts[1]), int(parts[2])
        mode = int(parts[3], 8) if len(parts) == 4 else None

        node, off, _rtype, _cnid = resolve(index, path)
        buf = bt.node(node)
        o = off + BSD_OFF
        old_uid, old_gid = struct.unpack_from(">II", buf, o)
        old_mode = struct.unpack_from(">H", buf, o + 10)[0]
        new_mode = old_mode if mode is None else (old_mode & 0o170000) | mode

        print("%s: %d:%d %06o -> %d:%d %06o"
              % (path, old_uid, old_gid, old_mode, uid, gid, new_mode))
        if a.dry_run:
            continue
        b = bytearray(buf)
        struct.pack_into(">II", b, o, uid, gid)
        struct.pack_into(">H", b, o + 10, new_mode)
        vol.catalog.write(node * bt.node_size, bytes(b))

    if not a.dry_run:
        print("flushed %d block(s)" % vol.flush())


if __name__ == "__main__":
    main()
