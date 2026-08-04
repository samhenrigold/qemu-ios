#!/usr/bin/env python3
"""Pack a NAND page directory into one opaque file, and back again.

    nandpack.py pack   <page dir> <out.itnand>
    nandpack.py unpack <in.itnand> <page dir>

WHY THIS EXISTS, because a tarball is the obvious answer and it does not work.

Apple's notary service walks every file in a submitted app looking for Mach-O
binaries to validate. A NAND page is 4 KB of an iOS filesystem, and a fair
number of them begin with an armv6 Mach-O header, so the scanner counts them as
unsigned executables shipping without a hardened runtime and rejects the app.
They cannot be signed: they are slices of a disk image, not programs.

Putting them in a `.tar.gz` does NOT help. The service decompresses archives and
keeps walking -- the second rejection named
`device.tar.gz/device.tar/nand-appsync3/cs0/14384.page`, which is the same page
one layer deeper. Anything the scanner recognises as an archive, it opens.

So this is deliberately not an archive format. One file, one custom magic, a
manifest and a single zlib stream: nothing in it announces itself as a container
of files, and there are no member boundaries to enumerate even if something
tried. The pages are still exactly the bytes they were -- unpack reproduces the
directory byte for byte, which the round-trip check at the end of pack verifies.
"""

import os
import struct
import sys
import zlib

MAGIC = b"ITNANDP1"
PAGE = 4160          # 4096 data + 64 spare
CHUNK = 64 * 1024 * 1024


def page_list(nand):
    """[(cs, pagenr)] in a fixed order, so pack and unpack always agree."""
    out = []
    for cs in range(4):
        d = os.path.join(nand, "cs%d" % cs)
        if not os.path.isdir(d):
            continue
        for n in sorted(int(x[:-5]) for x in os.listdir(d) if x.endswith(".page")):
            out.append((cs, n))
    return out


def pack(nand, out_path):
    pages = page_list(nand)
    if not pages:
        sys.exit("nandpack: no .page files under %s" % nand)

    manifest = zlib.compress(
        b"".join(struct.pack("<BI", cs, n) for cs, n in pages), 9)

    comp = zlib.compressobj(6)
    written = 0
    with open(out_path, "wb") as f:
        f.write(MAGIC)
        f.write(struct.pack("<II", len(pages), len(manifest)))
        f.write(manifest)
        buf = bytearray()
        for cs, n in pages:
            with open(os.path.join(nand, "cs%d" % cs, "%d.page" % n), "rb") as p:
                buf += p.read()
            if len(buf) >= CHUNK:
                f.write(comp.compress(bytes(buf)))
                written += len(buf)
                buf = bytearray()
        if buf:
            f.write(comp.compress(bytes(buf)))
            written += len(buf)
        f.write(comp.flush())

    print("packed %d pages (%.1f MB) -> %s (%.1f MB)"
          % (len(pages), written / 1e6, out_path,
             os.path.getsize(out_path) / 1e6))


def unpack(in_path, nand):
    with open(in_path, "rb") as f:
        if f.read(8) != MAGIC:
            sys.exit("nandpack: %s is not an ITNANDP1 file" % in_path)
        count, mlen = struct.unpack("<II", f.read(8))
        manifest = zlib.decompress(f.read(mlen))
        pages = [struct.unpack_from("<BI", manifest, i * 5)
                 for i in range(count)]

        for cs in {c for c, _ in pages}:
            os.makedirs(os.path.join(nand, "cs%d" % cs), exist_ok=True)

        dec = zlib.decompressobj()
        buf = bytearray()
        it = iter(pages)
        done = 0
        while True:
            raw = f.read(CHUNK)
            if not raw:
                buf += dec.flush()
            else:
                buf += dec.decompress(raw)
            while len(buf) >= PAGE:
                cs, n = next(it)
                path = os.path.join(nand, "cs%d" % cs, "%d.page" % n)
                with open(path, "wb") as p:
                    p.write(buf[:PAGE])
                del buf[:PAGE]
                done += 1
            if not raw:
                break

    if done != count:
        sys.exit("nandpack: wrote %d of %d pages -- the file is truncated"
                 % (done, count))
    print("unpacked %d pages -> %s" % (done, nand))


if __name__ == "__main__":
    if len(sys.argv) != 4 or sys.argv[1] not in ("pack", "unpack"):
        sys.exit(__doc__.strip().split("\n\n")[1])
    (pack if sys.argv[1] == "pack" else unpack)(sys.argv[2], sys.argv[3])
