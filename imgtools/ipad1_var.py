#!/usr/bin/env python3
"""Read a booted A4 device's volumes back off its NAND, for looking at /var after a run.

    ipad1_var.py extract NAND OVERLAY OUT.img PART [--geometry k48-16g]
        the latest copy of every user LPN (YaFTL spares: highest USN wins), base NAND + the QEMU
        nand-overlay's dirty pages, laid out as MBR partition PART (1-based) of a raw image
    ipad1_var.py ls IMG [N]       the N newest files of a raw HFS+ image: mtime, size, path
    ipad1_var.py cat IMG PATH     one file's data fork to stdout (PATH as ls prints it)
    ipad1_var.py --selfcheck

Found the 7.1.2 reboot loop: /.launchd_log_shutdown baked in, then var/log/com.apple.launchd read back here.
"""
import mmap
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ipad1_nand as n
import hfsvol


def extract(base, overlay, out, part, geometry="k48-16g"):
    geo = n.Geo(name=geometry, **n.GEOMETRIES[geometry])
    stride = geo.page_size + geo.spare_bytes

    def open_mm(path):
        with open(path, "rb") as f:
            return mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)

    src, best = {}, {}
    for cs in range(geo.num_cs):
        b, c = geo.cs_to_bus_ce(cs)
        dirty_path = os.path.join(overlay, "bus%d-ce%d.dirty" % (b, c)) if overlay else ""
        om = open_mm(os.path.join(overlay, "bus%d-ce%d.pages" % (b, c))) if dirty_path and os.path.exists(dirty_path) else None
        dirty = open(dirty_path, "rb").read() if om else b""
        src[cs] = (open_mm(os.path.join(base, "bus%d-ce%d.pages" % (b, c))), om, dirty)
    def record(cs, p):
        bm, om, dirty = src[cs]
        return om if dirty and dirty[p >> 3] & (1 << (p & 7)) else bm
    for cs in range(geo.num_cs):
        for p in range(geo.pages_per_ce):
            o = p * stride + geo.page_size
            sp = record(cs, p)[o:o + n.META]
            if not any(sp) or sp == b"\xff" * n.META:
                continue
            sp = n.whiten(sp, p)
            lpn, usn = struct.unpack_from("<II", sp)
            if sp[9] & n.T_USER and not sp[9] & n.T_INDEX and lpn != n.UNMAPPED:
                if lpn not in best or usn >= best[lpn][0]:
                    best[lpn] = (usn, cs, p)

    def page(lpn):
        if lpn not in best:
            return None
        _, cs, p = best[lpn]
        return record(cs, p)[p * stride:p * stride + geo.page_size]

    _, start, count = n.mbr_parts(page(0))[part - 1]
    with open(out, "wb") as f:
        f.truncate(count * geo.page_size)
        for i in range(count):
            d = page(start + i)
            if d is not None and any(d):
                f.seek(i * geo.page_size)
                f.write(d)
    return len(best)


class ImageVolume(hfsvol.Volume):
    """hfsvol's B-tree reader over a raw image file instead of NAND page files."""

    def __init__(self, path):
        self.f = open(path, "rb")
        hfsvol.Volume.__init__(self, None)

    def read_block(self, num):
        self.f.seek(num * hfsvol.BLOCK)
        return bytearray(self.f.read(hfsvol.BLOCK))


def files(img):
    """[(path, size, mtime, fork bytes)] of every file in the catalog."""
    vol = ImageVolume(img)
    bt = hfsvol.BTree(vol.catalog)
    names, out = {}, []
    for _, buf in bt.leaf_nodes():
        for rec in bt.records(buf):
            parent, name, body = hfsvol.cat_key(rec)
            typ = struct.unpack_from(">h", rec, body)[0]
            if typ == hfsvol.kHFSPlusFolderRecord:
                names[struct.unpack_from(">I", rec, body + 8)[0]] = (parent, name)
            elif typ == hfsvol.kHFSPlusFileRecord:
                out.append((parent, name, struct.unpack_from(">Q", rec, body + 88)[0],
                            struct.unpack_from(">I", rec, body + 16)[0], rec[body + 88:body + 168]))

    def path(cnid):
        parts = []
        while cnid in names and cnid != 2:
            cnid, nm = names[cnid]
            parts.append(nm)
        return "/".join(reversed(parts))
    return vol, [(path(p) + "/" + nm, size, mtime, fork) for p, nm, size, mtime, fork in out]


def selfcheck():
    """build() lays a synthetic data partition into the selfcheck geometry; extract() must give it back."""
    import argparse, random, shutil, tempfile
    work = tempfile.mkdtemp(prefix="ipad1_var_self.")
    ps, rnd = 4096, random.Random(2)
    mbr = bytearray(ps * 63)
    mbr[510:512] = b"\x55\xaa"
    for i, (typ, lba, cnt) in enumerate([(0xAF, 63, 700), (0xAE, 800, 4000), (0xAF, 763, 8)]):
        struct.pack_into("<BBHBBHII", mbr, 0x1be + 16 * i, 0, 0, 0, typ, 0, 0, lba, cnt)
    sysimg = bytearray(ps * 700)
    sysimg[1024:1026] = b"HX"
    data = bytearray(rnd.getrandbits(8) for _ in range(ps * 500))
    paths = {}
    for name, blob in (("mbr", mbr), ("system.img", sysimg), ("data.img", data)):
        paths[name] = os.path.join(work, name)
        open(paths[name], "wb").write(blob)
    nand = n.build(argparse.Namespace(geometry="selfcheck", mbr=paths["mbr"], system=paths["system.img"], s3=None,
                                      data=paths["data.img"], out=os.path.join(work, "nand"), force=True,
                                      kernelcache=None, kernel_version=b"Darwin Kernel Version selfcheck"))
    out = os.path.join(work, "var.img")
    extract(nand, None, out, 2, geometry="selfcheck")
    got = open(out, "rb").read()
    ok = got[:len(data)] == bytes(data) and not any(got[len(data):])
    shutil.rmtree(work, ignore_errors=True)
    print("ipad1_var selfcheck", "passed" if ok else "FAILED")
    return ok


def main(argv):
    if argv[:1] == ["--selfcheck"]:
        return 0 if selfcheck() else 1
    if argv[:1] == ["extract"] and len(argv) >= 5:
        geometry = argv[argv.index("--geometry") + 1] if "--geometry" in argv else "k48-16g"
        print("%d user LPNs; wrote %s" % (extract(argv[1], argv[2], argv[3], int(argv[4]), geometry), argv[3]))
        return 0
    if argv[:1] == ["ls"] and len(argv) in (2, 3):
        _, fl = files(argv[1])
        for p, size, mtime, _ in sorted(fl, key=lambda f: -f[2])[:int(argv[2]) if len(argv) == 3 else 60]:
            print(mtime, size, p)
        return 0
    if argv[:1] == ["cat"] and len(argv) == 3:
        vol, fl = files(argv[1])
        for p, size, _, fork in fl:
            if p == argv[2]:
                sys.stdout.buffer.write(hfsvol.Fork(vol, fork, 0).read(0, size))
                return 0
        sys.exit("%s: no such file" % argv[2])
    sys.exit(__doc__)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
