#!/usr/bin/env python3
"""Pack a NAND page-file directory into a single mmap-able image.

    pack_nand.py <nand-dir> <out.itnand>

The emulator's base NAND is a directory of one 4160-byte file per page
(4096 data + 64 spare), about 128,000 of them. Every guest page read is an
fopen/fread/fclose of one of those, which is slow everywhere and painful on
iOS, where the app also has to install all those files. Packed, the emulator
mmaps the image once and a page read becomes a memcpy.

It is smaller too: a 4160-byte file occupies two 4 KiB blocks, so the directory
costs ~1 GiB for ~533 MiB of pages.

Pages are sparse -- absent means "erased" -- so the index maps every possible
page to a record number, with 0 meaning absent.

Format, all little-endian:

    magic     8 bytes   "ITNAND01"
    page_size u32       4096
    spare_size u32      64
    num_cs    u32       number of chip selects
    pages_per_cs u32    index entries per chip select
    index     u32 * num_cs * pages_per_cs   1-based record number, 0 = absent
    records   (page_size + spare_size) each, in the order the index refers to
"""

import os
import struct
import sys

MAGIC = b"ITNAND01"
PAGE = 4096
SPARE = 64
RECORD = PAGE + SPARE


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    src, out = sys.argv[1], sys.argv[2]

    cs_dirs = sorted(
        (d for d in os.listdir(src) if d.startswith("cs") and d[2:].isdigit()),
        key=lambda d: int(d[2:]),
    )
    if not cs_dirs:
        sys.exit(f"{src}: no csN directories")

    # One pass to learn which pages exist and how big the index must be.
    pages = {}
    highest = 0
    for cs_index, d in enumerate(cs_dirs):
        entries = {}
        with os.scandir(os.path.join(src, d)) as it:
            for e in it:
                if not e.name.endswith(".page"):
                    continue
                stem = e.name[:-5]
                if not stem.isdigit():
                    continue
                n = int(stem)
                entries[n] = e.path
                highest = max(highest, n)
        pages[cs_index] = entries
        print(f"{d}: {len(entries)} pages")

    pages_per_cs = highest + 1
    num_cs = len(cs_dirs)
    index = [0] * (num_cs * pages_per_cs)

    header = struct.pack("<8sIIII", MAGIC, PAGE, SPARE, num_cs, pages_per_cs)
    index_bytes = len(index) * 4
    print(f"index: {num_cs} x {pages_per_cs} entries ({index_bytes / 2**20:.1f} MiB)")

    with open(out, "wb") as f:
        f.write(header)
        f.write(b"\0" * index_bytes)          # rewritten once record numbers are known

        record = 0
        for cs_index in range(num_cs):
            for page_nr in sorted(pages[cs_index]):
                with open(pages[cs_index][page_nr], "rb") as pf:
                    data = pf.read()
                # Short final records would silently shift every later offset.
                if len(data) < RECORD:
                    data = data + b"\0" * (RECORD - len(data))
                f.write(data[:RECORD])
                record += 1
                index[cs_index * pages_per_cs + page_nr] = record

        f.seek(len(header))
        f.write(struct.pack(f"<{len(index)}I", *index))

    size = os.path.getsize(out)
    print(f"wrote {out}: {record} pages, {size / 2**20:.1f} MiB")


if __name__ == "__main__":
    main()
