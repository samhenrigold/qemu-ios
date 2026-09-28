#!/usr/bin/env python3
"""The flash bookkeeping of a generated iPod touch 2G NAND image, from parameters instead of a template.

    ipod2g_nand.py check NAND --blocks N --epoch E   compare NAND's non-filesystem pages with the generated ones

build_nand.py lays one HFSX volume over the four chip-selects with ftlmap.predict(); every page that formula
does not cover used to be copied from a template image (nand-canonical). Those ~50 pages were themselves made
by a generator (the FTL context page still carries its log line, "Writing FTL Meta to physical page 255 @ cs 3"),
so they are regular and are rebuilt here:

  eb0 p0 cs0         blank page, spare[9] = 0x43
  eb0/eb1 map pages  the FTL's logical->virtual block map, identity + 1 (LBN n -> VBN n+1), 2048 u16 per page,
                     striped like the volume but over erase blocks 0/1 (FTL_MAP_PAGES pages, see map_page())
  eb1 VFL context    every other page of erase block 1 up to page 135 on every chip-select (vfl_page())
  cs3 p255           the FTL context: 20-VBN free pool 3..22 (the emulator moves it outside the volume at run
                     time, ipod_touch_fmss.c fmss_fix_generated_free_pool), per-block tables, the log line
  eb2 p256 cs0..2    device LBA 0..2: protective MBR, GPT header, one Apple_HFS entry sized to the volume
  eb4095 p0 cs0      NANDDRIVERSIGN: '0' + NAND epoch | 0x43313100, flags 4 (the epoch is Restore.plist's
                     DeviceMap SCEP: 1 on 2.x, 4 on 3.x/4.x; a 3.1.3 kernel refuses a '1' store)
  eb4095 p1 all cs   DEVICEINFOBBT: every block good
"""
import argparse
import binascii
import os
import struct
import sys

PAGE, SPARE = 4096, 64
PPB = 128                                  # pages per erase block
BBT_PAGE = 4095 * PPB                      # eb4095
BLANK_SPARE = b"\x00" * 8 + b"\xff\x00\xff\x00" + b"\x00" * 52
FTL_MAP_PAGES = 18                         # 18 x 2048 map entries
VFL_SPARE = b"\x01" + b"\x00" * 8 + b"\x80" + b"\x00" * 54
META_SPARE = b"\x00" * 9 + b"\x43" + b"\x00" * 54
GPT_SLACK = 11                             # LBAs the partition holds beyond the volume (as generated)
HFS_TYPE = bytes.fromhex("005346480000aa11aa1100306543ecac")   # 48465300-0000-11AA-AA11-00306543ECAC
FTL_LOG = b"Writing FTL Meta to physical page 255 @ cs 3\nto physical page 130 @ cs 2\n"


def u16s(words):
    return struct.pack("<%dH" % len(words), *words).ljust(PAGE, b"\0")


def stripe(k):
    """(cs, page) of the k-th map page: the volume's interleave over erase blocks 0/1."""
    m = k + 1
    r = m // 4
    return m % 4, ((r + 1) % 2) * PPB + (r + 1) // 2


def map_page(k):
    return u16s([k * 2048 + i + 1 for i in range(2048)])


def vfl_page(cs, first):
    w = [0] * 2048
    w[0], w[6] = cs, 1
    w[19:839] = [0xFFF0] * 820
    w[839:848] = [0, 1, 2, 3, 0x800, 0x800, 0, 1, 2]
    w[877], w[878], w[1018] = 0x14, 0x10, 2
    if first:
        w[1024], w[1028] = 1, 0x8000
    return u16s(w)


def ftl_context():
    w = [0] * 2048
    w[4] = 20
    w[7:27] = range(3, 23)                            # free pool VBNs
    for i, v in enumerate(range(5, 23)):              # (u16 v, u16 0) pairs
        w[28 + 2 * i] = v
    for i in range(18):
        w[212 + 10 * i] = 0xFFFF
    w[1021:1024] = [0x4656, 0xFFFF, 0xB9A9]
    data = bytearray(u16s(w))
    data[2048:2048 + len(FTL_LOG)] = FTL_LOG
    return bytes(data)


def gpt_pages(blocks):
    end = 3 + blocks - 1 + GPT_SLACK
    mbr = bytearray(PAGE)
    mbr[8], mbr[10] = 0xFF, 0xFF                      # as generated: a spare pattern in the data area
    struct.pack_into("<BBBBBBBBII", mbr, 0x1BE, 0, 0, 0, 0, 0xEE, 0, 0, 0, 3, blocks + 10)
    mbr[0x1FE:0x200] = b"\x55\xaa"
    ent = bytearray(PAGE)
    ent[0:16] = HFS_TYPE
    struct.pack_into("<QQ", ent, 0x20, 3, end)
    hdr = bytearray(PAGE)
    hdr[0:8] = b"EFI PART"
    struct.pack_into("<III", hdr, 8, 0x00010000, 0x5C, 0)
    struct.pack_into("<QIII", hdr, 0x48, 2, 1, 0x80, binascii.crc32(bytes(ent[:0x80])) & 0xFFFFFFFF)
    struct.pack_into("<I", hdr, 0x10, binascii.crc32(bytes(hdr[:0x5C])) & 0xFFFFFFFF)
    return [bytes(mbr), bytes(hdr), bytes(ent)]


def nand_signature(epoch):
    data = bytearray(PAGE)
    data[0:14] = b"NANDDRIVERSIGN"
    struct.pack_into("<II", data, 0x34, 4, 0x43313100 | (0x30 + epoch))
    return bytes(data)


def bbt():
    return b"DEVICEINFOBBT".ljust(16, b"\0") + b"\xff" * 4080


def metadata_pages(blocks, epoch):
    """{(cs, page): data + spare} for every page build_nand.py's volume formula does not cover."""
    pages = {(0, 0): bytes(PAGE) + META_SPARE, (3, 255): ftl_context() + META_SPARE}
    maps = {stripe(k) for k in range(FTL_MAP_PAGES)}
    for k in range(FTL_MAP_PAGES):
        pages[stripe(k)] = map_page(k) + bytes(SPARE)
    for cs in range(4):
        for pg in range(PPB, PPB + 8):
            if (cs, pg) not in maps:
                pages[(cs, pg)] = vfl_page(cs, cs == 0 and pg > PPB) + VFL_SPARE
        pages[(cs, BBT_PAGE + 1)] = bbt() + bytes(SPARE)
    for cs, data in enumerate(gpt_pages(blocks)):
        pages[(cs, 2 * PPB)] = data + BLANK_SPARE
    pages[(0, BBT_PAGE)] = nand_signature(epoch) + bytes(SPARE)
    return pages


def write_metadata(out, blocks, epoch):
    for (cs, pg), rec in metadata_pages(blocks, epoch).items():
        d = os.path.join(out, "cs%d" % cs)
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, "%d.page" % pg), "wb") as f:
            f.write(rec)


def check(nand, blocks, epoch):
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from ftlmap import predict
    covered = {predict(n) for n in range(blocks)}
    want = metadata_pages(blocks, epoch)
    have = {}
    for cs in range(4):
        for name in os.listdir(os.path.join(nand, "cs%d" % cs)):
            if name.endswith(".page") and (cs, int(name[:-5])) not in covered:
                have[(cs, int(name[:-5]))] = open(os.path.join(nand, "cs%d" % cs, name), "rb").read()
    bad = 0
    for key in sorted(set(want) | set(have)):
        a, b = want.get(key), have.get(key)
        if a != b:
            bad += 1
            detail = "missing" if b is None else "extra" if a is None else "bytes %s" % [
                hex(i) for i in range(len(a)) if a[i] != b[i]][:8]
            print("cs%d p%d: %s" % (key + (detail,)))
    print("%d/%d metadata pages identical" % (len(want) - bad, len(want)))
    return bad


def selfcheck():
    p = metadata_pages(128000, 1)
    assert len(p) == 50 and all(len(v) == PAGE + SPARE for v in p.values())
    hdr = p[(1, 256)]
    assert binascii.crc32(hdr[:0x10] + b"\0" * 4 + hdr[0x14:0x5C]) & 0xFFFFFFFF == struct.unpack_from("<I", hdr, 0x10)[0]
    assert struct.unpack_from("<Q", p[(2, 256)], 0x28)[0] == 128013


if __name__ == "__main__":
    selfcheck()
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("check")
    c.add_argument("nand")
    c.add_argument("--blocks", type=int, required=True)
    c.add_argument("--epoch", type=int, required=True)
    a = ap.parse_args()
    sys.exit(1 if check(a.nand, a.blocks, a.epoch) else 0)
