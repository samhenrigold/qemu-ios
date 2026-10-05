#!/usr/bin/env python3
"""Offline NAND image generator for the iPad 1 (K48AP) / iOS 3.2.2 (7B500) kernel.

    ipad1_nand.py mbr [--geometry k48-16g] [--system-mib 1280] OUT     (see make_mbr)
    ipad1_nand.py build --geometry k48-16g --mbr mbr.bin --kernelcache kernelcache.mach \
                        --system rdisk0s1-system.img [--s3 rdisk0s3.bin] \
                        [--data SIZE|IMAGE|none] --out FILE-or-DIR
    ipad1_nand.py check DIR [--mbr FILE] [--system IMG]
    ipad1_nand.py --selfcheck

The kernel's NAND stack (AppleNANDFTL: FPart -> VSVFL -> YaFTL, all talking to
the IOP through AppleS5L8920XIOPFMI) never formats a device by itself unless
booted with nand-enable-reformat=1, and even then it leaves an empty FTL. This
script writes, offline, exactly the on-flash state such a device is in after a
restore and a power cut *before the FTL context was flushed*: special pages,
VFL contexts, closed user/index blocks with BTOCs, and NO CX01 context. YaFTL
then takes its normal "CXT is not valid. Performing full NAND R/O restore"
path, rebuilds its RAM state from page spares/BTOCs (no writes needed - the
index pages are already on flash and newer than every user block), and mounts
the MBR + HFS+ volumes at the logical block addresses the real iPad used.

Store format (directory, shared with the IOP model in hw/arm/s5l8930_iop.c)
--------------------------------------------------------------------------
    geometry.json   {"page_bytes":4096,"spare_bytes":128,"pages_per_block":128,
                     "blocks_per_ce":4096,"ce_per_bus":4,"buses":2,
                     "chip_id":"0xB614D5AD"}
    bus<b>-ce<c>.pages  one sparse file per physical chip select; page at a
                    fixed stride of page_bytes+spare_bytes, data first, then the
                    spare (12-byte YaFTL/VFL meta in spare[0..12), rest 0x00);
                    page index = block*pages_per_block + page; file length =
                    blocks_per_ce*pages_per_block*stride; unwritten pages are
                    holes (read as zeros). Every written page has a non-zero
                    spare byte, so "record all zero or all 0xFF" == blank page.
Kernel chip-select (CS/virtual CE) n lives at bus n // ce_per_bus, ce n %
ce_per_bus (bus-major). Evidence: in the kernel's own self-format store
(nand/selfformat, 2x2 CE) the CX01 context pages land at vpn k -> CS k%4, and
the block-statuses page (vpn 5) is on bus0-ce1, the stats page (vpn 3) on
bus1-ce1 - impossible under round-robin. See cs_to_bus_ce().

Whitening / AES: page DATA is stored plain (the AES step lives in the IOP
command and the model ignores it). The 10 META bytes of every VFL/FTL page are
stored WHITENED, exactly as the kext does on this hardware: XOR of the three
meta words with LCG_TABLE[(word + physical_page) % 256] (seed 0x50F4546A,
c04e71ac constants), and NANDDRIVERSIGN flags = 0x00010005 (whitening bit
set, as DT metadata-whitening=1 makes the kernel write). The kernel's own
self-format store proved both: its VFLCxt spare on flash decodes with that
table to {FF x8, 00, 80}. Special pages keep the raw 0xA5 meta the kext
writes for them. Booting a plain-meta image with flags 0x5 failed in
_LoadVFLCxt (VSVFL line 1103: no block 1..0xC8 has a readable page 0 with
spare[8]==0, spare[9]==0x80) - the kext whitened on read regardless. NANDDRIVERSIGN flags = 0x5 with
the metadata-whitening bit (0x10000) clear; WMR_Start (7B500 c07f3d20) takes
whitening from that flag alone, so meta is never XORed. The IOP model must
treat the AES key-type field of the read/write commands as a no-op.

Geometry: see GEOMETRIES. The real 16 GB unit exports 3,925,449 4 KiB sectors
(measured with dd over ssh); 7B500 YAFTL_Init reproduces that number exactly
for 8 CE x 0xB614D5AD (4096 B pages), and for nothing else in iBoot's table.

Every structure below was checked against the 7B500 kernelcache; the address
in each comment is where it is validated or written.
"""

import argparse
import array
import mmap
import os
import shutil
import struct
import subprocess
import sys
import tempfile

META = 12
ERASED_META = b"\xff" * META

# NANDDRIVERSIGN: nSig = '0'+PE_nand_epoch(1) | 0x43313100 (c07f3cfe), flags 5
# (c07f3f14; +0x10000 only when formatting with DT whitening), then up to 0xff
# bytes of the kernel version string (c07f3f2a), never validated by the reader;
# `build` takes it from the kernelcache (kernel_version()).
# The epoch is the IPSW's (Restore.plist DeviceMap SCEP): 1 on the K48 and N18, 3 on the N88. A store
# whose epoch is behind the kernel's makes WMR wait for an "epoch roll" only restored performs.
EPOCH = 1


def nsig():
    return 0x43313100 | (0x30 + EPOCH)
SIG_FLAGS = 0x00010005
# Boards whose DT has no metadata-whitening (S5L8920/8922: N18, N88) neither write nor accept whitened
# meta (WMR "Metadata whitening not supported"): `build --no-whitening` stores it plain, flags 0x4: what
# those boards' own FIL writes when it formats (3.1.3 N88 7E18, read back off its store), and the only value
# 3.1.3 accepts (it refuses flags > 4 as "Incompatible Signature", 0xc05c58aa); 4.x and 5.x take it too.
WHITENING = True


# 3.1.x's driver (N18 7E18 AppleNANDFTL: 0xc03dbae2 writes 4) formats with flags 4 and refuses a store
# whose flags exceed 4 under a '1' second signature byte (0xc03db8aa, "Incompatible Signature"): `--sig-flags 4`.
SIG_FLAGS_OVERRIDE = None


def sig_flags():
    if SIG_FLAGS_OVERRIDE is not None:
        return SIG_FLAGS_OVERRIDE
    return SIG_FLAGS if WHITENING else SIG_FLAGS & ~0x10000


def nand_epoch(kernelcache):
    """PE_nand_epoch as the kernel answers it: 1 up to IOFlashStorage 410.3 (iOS 4.3.0), 2 from 410.4 (4.3.5); the FIL
    compares nSig's low byte with '0' + it and, on a mismatch, waits for a SecureRoot epoch roll (a restore). Found by
    the call's shape, as FirmwareKit's K48NAND.signatureEpoch: ldr rN, [pc]; blx rN; adds r0, #0x30; uxtb r2, r0;
    pop {r7, pc}, the callee starting movs r0, #N. 1 when absent."""
    d = open(kernelcache, "rb").read()
    segs, off = [], 28
    for _ in range(struct.unpack_from("<I", d, 16)[0]):
        cmd, size = struct.unpack_from("<II", d, off)
        if cmd == 1:
            vmaddr, vmsize, fileoff = struct.unpack_from("<III", d, off + 24)
            segs.append((vmaddr, vmsize, fileoff))
        off += size
    i = d.find(b"\x30\x30\xc0\xb2\x80\xbd")
    while i >= 4:
        if d[i - 3] & 0xF8 == 0x48 and d[i - 2] == 0x80 | (d[i - 3] & 7) << 3 and d[i - 1] == 0x47:
            pool = ((i - 4) & ~3) + 4 + d[i - 4] * 4
            target = struct.unpack_from("<I", d, pool)[0] & ~1
            for vmaddr, vmsize, fileoff in segs:
                o = fileoff + target - vmaddr
                if vmaddr <= target < vmaddr + vmsize and o + 2 <= len(d) and d[o + 1] == 0x20:   # movs r0, #N
                    return d[o]
        i = d.find(b"\x30\x30\xc0\xb2\x80\xbd", i + 1)
    return 1


def kernel_version(kernelcache):
    """The "Darwin Kernel Version ..." string (7B500: 0xc0214690) out of a decrypted kernelcache."""
    data = open(kernelcache, "rb").read()
    at = data.find(b"Darwin Kernel Version ")
    if at < 0:
        raise SystemExit("%s: no Darwin Kernel Version string" % kernelcache)
    return data[at:data.index(b"\0", at)][:0xff]

# YaFTL SpareData.type (yaftl_common.h; 7B500 c080486e / c0803d62 / c0803664)
T_INDEX, T_CLOSED, T_USER, T_CXT, T_VFL = 0x4, 0x8, 0x10, 0x20, 0x80
UNMAPPED = 0xFFFFFFFF


def _lcg_table():
    t, v = [], 0x50F4546A
    for _ in range(256):
        for _ in range(763):
            v = (0x19660D * v + 0x3C6EF35F) & 0xFFFFFFFF
        t.append(v)
    return t


LCG_TABLE = _lcg_table()


def whiten(meta, ppage):
    """XOR the 12-byte meta with the page-indexed LCG table (self-inverse); plain when WHITENING is off."""
    if not WHITENING:
        return meta[:10] + b"\0\0"
    w = struct.unpack("<3I", meta[:12])
    # bytes 10-11 never travel through the meta DMA; the kext leaves 00 00 on flash
    return struct.pack("<3I", *[w[i] ^ LCG_TABLE[(i + ppage) % 256] for i in range(3)])[:10] + b"\0\0"

GEOMETRIES = {
    # iBoot-817.29 chip table (iBoot+0x29234) row for 0xB614D5AD: 0x1000 blocks,
    # 128 pages, 4096 B, spare 0x80; board row 16 = 2 buses x 4 CE, vendor
    # 0x100014 (2 VFL banks per CE, c07f9300-style even/odd interleave).
    "k48-16g": dict(chip_id=bytes.fromhex("add514b6") + b"\0" * 4, num_bus=2,
                    ce_per_bus=4, blocks_per_ce=0x1000, pages_per_block=128,
                    page_size=4096, spare_bytes=0x80, vendor_type=0x100014),
    # The 16 GB part as 3.0's AppleS5L8920XIOPFMI table lists it on 2 buses x 4 CEs: vendor type 0x10001,
    # one VFL bank per CE, so 1024-page superblocks whose block TOC fits one page. 3.0's yaFTL assumes one
    # (YAFTL_Init c05c74ec on N88 7A341; fixed in 3.1), so k48-16g's 2048-page superblocks restore garbage.
    "k48-16g-v1": dict(chip_id=bytes.fromhex("add514b6") + b"\0" * 4, num_bus=2,
                       ce_per_bus=4, blocks_per_ce=0x1000, pages_per_block=128,
                       page_size=4096, spare_bytes=0x80, vendor_type=0x10001),
    # the research docs' guess (Samsung K9LCG08U1M, 8 KiB pages); the kernel
    # self-format oracle in nand/selfformat uses it. Not the captured unit: its
    # MBR is 4 KiB-sectored, so `build` refuses this geometry.
    "k48-16g-7294D7EC": dict(chip_id=bytes.fromhex("ecd79472") + b"\0" * 4, num_bus=2,
                             ce_per_bus=2, blocks_per_ce=0x1038, pages_per_block=128,
                             page_size=8192, spare_bytes=0x1b4, vendor_type=0x100014),
    # tiny synthetic geometry for --selfcheck (same math, 16-page blocks)
    "selfcheck": dict(chip_id=b"\xad\xd5\x14\xb6\0\0\0\0", num_bus=2,
                      ce_per_bus=2, blocks_per_ce=0x1000, pages_per_block=16,
                      page_size=4096, spare_bytes=0x80, vendor_type=0x100014),
}


class Geo:
    def __init__(self, **kw):
        self.__dict__.update(kw)
        self.num_cs = self.num_bus * self.ce_per_bus
        self.pages_per_ce = self.blocks_per_ce * self.pages_per_block
        # VSVFL (VSVFL_Init c07f97fc-style): 2 banks/CE for vendor 0x100014, 1 for 0x10001 (the vendor
        # type 3.0's AppleS5L8920XIOPFMI table gives this part on 2 buses x 4 CEs)
        self.vfl_banks = 2 if self.vendor_type == 0x100014 else 1
        self.banks_total = self.num_cs * self.vfl_banks
        self.blocks_per_bank = self.blocks_per_ce // self.vfl_banks
        self.ppsublk = self.pages_per_block * self.banks_total
        # VSVFL_Format c07fe5e8: usable = (0x3D0 << (bits(blocks_per_bank)-1-10))
        s = self.blocks_per_bank.bit_length() - 1 - 10
        self.usable = (0x3D0 << s) if s >= 0 else (0x3D0 >> -s)
        self.pool = self.blocks_per_bank - self.usable
        # YAFTL_Init c0806d58: geometry derived exactly as the kernel does
        bpp = self.page_size
        self.num_blocks = self.usable
        self.toc = -(-4 * self.ppsublk // bpp)                 # tocPagesPerBlock
        self.toc_entries = bpp // 4
        self.data_pages = self.ppsublk - self.toc
        self.num_iblocks = 3 * -(-((self.num_blocks - 8) * self.data_pages)
                                 // (self.data_pages * self.toc_entries))
        self.total_pages = ((self.num_blocks - 8) * self.data_pages
                            - self.num_iblocks * self.ppsublk)
        self.exported_pages = (self.total_pages - 1) // 100 * 99   # c08038bc [0x120]=0x63
        self.toc_array_len = -(-self.ppsublk * self.num_blocks * 4 // bpp)
        # special-block candidates (VFL_ProductionFormat c07fea2c..): CS0 gets 5
        # (BBT, BBT, UNIQUEINFO x2, NANDDRIVERSIGN), other CS 2, from the top down.
        top = self.blocks_per_ce - 1
        self.cand = {cs: [top - i for i in range(5 if cs == 0 else 2)]
                     for cs in range(self.num_cs)}
        self.bbt_len = -(-self.blocks_per_ce // 8)
        # VFL context blocks: first four good blocks >= block offset 1 (c07fe6b8)
        self.vfl_blocks = [1, 2, 3, 4]
        self.ctrl_blocks = [0, 1, 2]                         # c07fe660
        # _ReplaceBadBlk(cs, pbn) for pbn 0..vfl_blocks[3] (c07fe7ee): each takes
        # the next pool slot of its own bank -> remap physical block -> pool block
        self.remap, slots = {}, [0] * self.vfl_banks
        for pbn in range(self.vfl_blocks[3] + 1):
            bank = pbn % self.vfl_banks
            self.remap[pbn] = (bank, slots[bank])
            slots[bank] += 1
        self.replaced_count = slots

    def pool_pblock(self, bank, slot):
        return self.vfl_banks * (self.usable + slot) + bank

    def cs_to_bus_ce(self, cs):
        return cs // self.ce_per_bus, cs % self.ce_per_bus

    def ppage(self, pblock, page):
        """physical page number inside a CS (special pages, VFL context: no remap)"""
        return pblock * self.pages_per_block + page

    def phys(self, cs, pblock, page):
        """FTL view: physical page after the reserved-pool remap of blocks 0..4"""
        if pblock in self.remap:
            pblock = self.pool_pblock(*self.remap[pblock])
        return self.ppage(pblock, page)

    def vpn_to_phys(self, vpn):
        """YaFTL vpn -> (cs, physical page). vsvfl.c:197-217 + C2P c07f9300."""
        vblock, j = divmod(vpn, self.ppsublk)
        bank, page = j % self.banks_total, j // self.banks_total
        cs, bit = bank % self.num_cs, bank // self.num_cs
        return cs, self.phys(cs, self.vfl_banks * vblock + bit, page)


# --- store -----------------------------------------------------------------

class Store:
    def __init__(self, path, geo, create=False):
        import json
        self.geo, self.path = geo, path
        self.stride = geo.page_size + geo.spare_bytes
        self.nrec = 0
        names = [(b, c) for b in range(geo.num_bus) for c in range(geo.ce_per_bus)]
        if create:
            os.makedirs(path, exist_ok=True)
            with open(os.path.join(path, "geometry.json"), "w") as f:
                json.dump(dict(page_bytes=geo.page_size, spare_bytes=geo.spare_bytes,
                               pages_per_block=geo.pages_per_block, blocks_per_ce=geo.blocks_per_ce,
                               ce_per_bus=geo.ce_per_bus, buses=geo.num_bus,
                               chip_id="0x%08X" % struct.unpack("<I", geo.chip_id[:4])[0],
                               # only off the part's default, so existing stores are unchanged (FirmwareKit K48NAND too)
                               **({} if geo.vendor_type == 0x100014 else {"vendor_type": geo.vendor_type})), f, indent=1)
            self.files = {}
            for b, c in names:
                f = open(os.path.join(path, "bus%d-ce%d.pages" % (b, c)), "w+b")
                f.truncate(geo.pages_per_ce * self.stride)
                self.files[(b, c)] = f
        else:
            g = json.load(open(os.path.join(path, "geometry.json")))
            want = dict(page_bytes=geo.page_size, spare_bytes=geo.spare_bytes,
                        pages_per_block=geo.pages_per_block, blocks_per_ce=geo.blocks_per_ce,
                        ce_per_bus=geo.ce_per_bus, buses=geo.num_bus)
            if any(g.get(k) != v for k, v in want.items()):
                raise SystemExit("%s: geometry.json %s does not match %s" % (path, g, geo.name))
            self.mm = {}
            for b, c in names:
                f = open(os.path.join(path, "bus%d-ce%d.pages" % (b, c)), "rb")
                self.mm[(b, c)] = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)

    def write(self, cs, ppage, data, meta, raw=False):
        assert len(data) == self.geo.page_size and len(meta) == META
        f = self.files[self.geo.cs_to_bus_ce(cs)]
        f.seek(ppage * self.stride)
        if not raw:
            meta = whiten(meta, ppage)
        rec = data + meta.ljust(self.geo.spare_bytes, b"\0")      # kext leaves spare[12..] = 0
        assert any(rec[self.geo.page_size:]), "spare must not be all zero (hole == blank)"
        f.write(rec)
        self.nrec += 1

    def close(self):
        for f in self.files.values():
            f.close()

    def read(self, cs, ppage, raw=False):
        """(data, 12-byte meta) with the meta un-whitened unless raw; (None, FF) if blank"""
        mm = self.mm[self.geo.cs_to_bus_ce(cs)]
        o = ppage * self.stride
        rec = mm[o:o + self.stride]
        sp = rec[self.geo.page_size:]
        if not any(sp) or sp == b"\xff" * len(sp) and rec[:self.geo.page_size] == b"\xff" * self.geo.page_size:
            return None, ERASED_META
        return rec[:self.geo.page_size], sp[:META] if raw else whiten(sp[:META], ppage)

    def read_vpn(self, vpn):
        return self.read(*self.geo.vpn_to_phys(vpn))


# --- on-flash structures ------------------------------------------------------

def spare(lpn, usn, typ):
    # SpareData {u32 lpn, u32 usn, u8 field_8, u8 type, u16 0xFFFF}; only 10
    # bytes travel through the IOP meta DMA, bytes 10-11 read back as 0xFF.
    return struct.pack("<IIBB", lpn, usn, 0, typ) + b"\xff\xff"


def special_page(geo, magic, hdr_ver, cands, payload):
    # header 0x38 B (_WriteSpecialBlock c07fd37c-style; reader c07ffa20 checks
    # the 16-byte magic, reads the payload length at +0x34, and on CS0's
    # DEVICEINFOBBT registers +0x14.. as the candidate blocks: +0x18 BBT copy,
    # +0x1c/+0x20 DEVICEUNIQUEINFO, +0x24 NANDDRIVERSIGN, +0x28/+0x2c DIAG).
    c = (list(cands) + [0xFFFFFFFF] * 8)[:8]
    hdr = magic.ljust(16, b"\0") + struct.pack("<I8II", hdr_ver, *c, len(payload))
    assert len(hdr) == 0x38 and len(hdr) + len(payload) <= geo.page_size
    # spare: the kext fills 0xA5 for special pages (oracle); never validated
    return (hdr + payload).ljust(geo.page_size, b"\0"), b"\xa5" * 10 + b"\0\0"


def bbt_bitmap(geo, cs):
    # bit = 1 good (vsvfl.c:829); block 0 and the candidates are marked bad
    bad = {0} | set(geo.cand[cs])
    bits = bytearray(b"\xff" * geo.bbt_len)
    for b in range(geo.blocks_per_ce):
        if b in bad or b >= geo.blocks_per_ce:
            bits[b // 8] &= ~(1 << (b % 8)) & 0xff
    return bytes(bits)


def vfl_context(geo, cs):
    # vfl_vsvfl_context_t, 0x800 B (vsvfl.c:5-35); values as VSVFL_Format
    # c07fe590.. and _WriteVFLCxtToFlash c07fc7b4 leave them on a fresh device
    c = bytearray(0x800)
    struct.pack_into("<IIIHH", c, 0, cs + 1, 0xFFFFFFFF, 2, 0, 8)  # usn_inc, usn_dec, ftl_type, usn_block, usn_page(+8 before copy, c07fc82c)
    struct.pack_into("<H", c, 0x10, 1 + len(geo.cand[cs]))            # BBT-bad count (c07fe69a)
    for bank, n in enumerate(geo.replaced_count):
        struct.pack_into("<H", c, 0x16 + 2 * bank, n)
    pool_map = [0xFFF0] * (geo.vfl_banks * geo.pool)                 # unused spare
    bad = {0} | set(geo.cand[cs])
    for pbn in bad:
        if pbn >= geo.vfl_banks * geo.usable:                         # bad spare
            bank, slot = pbn % geo.vfl_banks, pbn // geo.vfl_banks - geo.usable
            pool_map[bank * geo.pool + slot] = 0xFFFF
    for pbn, (bank, slot) in geo.remap.items():                       # replaced block
        pool_map[bank * geo.pool + slot] = pbn
    struct.pack_into("<%dH" % len(pool_map), c, 0x26, *pool_map)
    struct.pack_into("<4H", c, 0x68e, *geo.vfl_blocks)
    struct.pack_into("<HH3H", c, 0x696, geo.usable, geo.usable, *geo.ctrl_blocks)
    struct.pack_into("<I", c, 0x6da, geo.vendor_type)
    struct.pack_into("<I", c, 0x7f4, 2)                               # version (<=2, c07fc186)
    return vfl_checksum(c)


def vfl_checksum(c):
    # c07f8e94: sum/xor of the first 0x7F8 bytes as u32, +/^ 0xAABBCCDD
    words = struct.unpack_from("<%dI" % (0x7f8 // 4), c, 0)
    x = 0
    for w in words:
        x ^= w
    struct.pack_into("<II", c, 0x7f8, (sum(words) + 0xAABBCCDD) & 0xFFFFFFFF, x ^ 0xAABBCCDD)
    return bytes(c)


def write_metadata(st, geo, kernel_ver, nsig=None):
    for cs in range(geo.num_cs):
        cands = geo.cand[cs]
        pg = special_page(geo, b"DEVICEINFOBBT", 4, cands, bbt_bitmap(geo, cs))
        for blk in cands[:2]:                                # two BBT copies (c07fee14)
            for p in range(geo.pages_per_block):             # kernel fills the whole block with copies
                st.write(cs, geo.ppage(blk, p), *pg, raw=True)
        ctx = vfl_context(geo, cs)
        meta = struct.pack("<I", 0xFFFFFFFF) + b"\xff" * 4 + b"\x00\x80\xff\xff"   # c07fc85a..c07fc874
        for p in range(8):                                   # 8 identical copies, block 1 pages 0-7
            st.write(cs, geo.ppage(geo.vfl_blocks[0], p), ctx.ljust(geo.page_size, b"\0"), meta)
    sig = special_page(geo, b"NANDDRIVERSIGN", 0, [0] * 8, struct.pack("<II", nsig or globals()["nsig"](), sig_flags()) + kernel_ver.ljust(0x100, b"\0"))
    for p in range(geo.pages_per_block):
        st.write(0, geo.ppage(geo.cand[0][4], p), *sig, raw=True)


class FTLWriter:
    """Lays out user pages (then index pages) into consecutive YaFTL vblocks."""

    def __init__(self, st, geo):
        self.st, self.geo = st, geo
        self.vblock = 3                        # 0..2 are the (erased) ctrl blocks
        self.j = 0
        self.usn = 1
        self.btoc = []
        self.toc = {}                          # toc page -> array of vpn

    def _open_block(self):
        if self.vblock >= self.geo.num_blocks:
            raise SystemExit("image does not fit: needs more than %d vblocks" % self.geo.num_blocks)
        self.j, self.btoc = 0, []

    def put(self, lpn, data, typ):
        g = self.geo
        if self.j == 0:
            self._open_block()
        vpn = self.vblock * g.ppsublk + self.j
        self.st.write(*g.vpn_to_phys(vpn), data, spare(lpn, self.usn, typ))
        self.btoc.append(lpn)
        self.j += 1
        if self.j == g.data_pages:
            self._close_block(typ)
        return vpn

    def _close_block(self, typ):
        # BTOC: last `toc` pages hold u32 lpn per page (YAFTL_closeLatestBlock
        # c0803642: buffer memset 0xFF, spare type 8, index blocks 0xC)
        g = self.geo
        table = struct.pack("<%dI" % len(self.btoc), *self.btoc).ljust(g.toc * g.page_size, b"\xff")
        for i in range(g.toc):
            vpn = self.vblock * g.ppsublk + self.j + i
            self.st.write(*g.vpn_to_phys(vpn), table[i * g.page_size:(i + 1) * g.page_size],
                          spare(UNMAPPED, self.usn, T_CLOSED | (T_INDEX if typ == T_INDEX else 0)))
        self.vblock += 1
        self.usn += 1
        self.j = 0

    def next_block(self):
        """leave the current (partial) block open and start a new one"""
        if self.j:
            self.vblock += 1
            self.usn += 1
            self.j = 0

    def user(self, lpn, data):
        vpn = self.put(lpn, data, T_USER)
        t, i = divmod(lpn, self.geo.toc_entries)
        if t not in self.toc:
            self.toc[t] = array.array("I", [UNMAPPED] * self.geo.toc_entries)
        self.toc[t][i] = vpn

    def index_pages(self):
        # index pages: type 0x4, lpn = TOC page number, data = tocEntries u32 vpns
        # (writeIndexPage c0803d62); usn above every user block so the R/O
        # restore finds the TOC on flash already up to date.
        self.next_block()
        for t in sorted(self.toc):
            self.put(t, self.toc[t].tobytes(), T_INDEX)
        self.next_block()


# --- inputs ------------------------------------------------------------------

MBR_ENTRY = struct.Struct("<BBHBBHII")    # status, chs, type, chs, lba, count (chs squeezed)


def make_mbr(geo, system_mib):
    """The logical disk's head as a 7B500 restore leaves it on a K48, up to partition 1 (LBA 63): sector 0 is only
    the table (no boot code), the rest zero. p1 Apple_HFS system at 63, p3 an 8-sector 0xAF stub one sector past
    its end, p2 0xAE data 45 sectors after p3's start, to 45 sectors before the exported end. Start CHS is
    01 01 00 for LBA 63, FE FF FF past the CHS limit, end CHS always FE FF FF.
    ponytail: the gaps (1, 37 and the trailing 45) are measured on one 16 GB unit, not derived."""
    ps, n = geo.page_size, system_mib * (1 << 20) // geo.page_size
    head = bytearray(63 * ps)
    p3 = 63 + n + 1
    for i, (typ, lba, cnt) in enumerate([(0xAF, 63, n), (0xAE, p3 + 45, geo.exported_pages - (p3 + 45) - 45),
                                         (0xAF, p3, 8)]):
        chs = bytes.fromhex("010100" if lba == 63 else "feffff")
        head[0x1be + 16 * i:0x1ce + 16 * i] = b"\0" + chs + bytes([typ]) + b"\xfe\xff\xff" + struct.pack("<II", lba, cnt)
    head[510:512] = b"\x55\xaa"
    return bytes(head)


def mbr_parts(mbr):
    out = []
    for i in range(4):
        e = mbr[0x1be + 16 * i:0x1be + 16 * i + 16]
        typ = e[4]
        lba, cnt = struct.unpack_from("<II", e, 8)
        out.append((typ, lba, cnt))
    return out


def set_part(mbr, i, typ=None, cnt=None):
    o = 0x1be + 16 * i
    if typ is not None:
        mbr[o + 4] = typ
    if cnt is not None:
        struct.pack_into("<I", mbr, o + 12, cnt)


def parse_size(s):
    mult = {"k": 1 << 10, "m": 1 << 20, "g": 1 << 30}
    return int(float(s[:-1]) * mult[s[-1].lower()]) if s[-1].lower() in mult else int(s)


def make_hfs_image(path, size, block_size=None, journaled=True):
    """Bare (no partition map) case-sensitive journaled HFS+, like iOS's data volume, in a SPARSE raw
    file: newfs_hfs writes only the volume's metadata, so a full-size (14.7 GB) data partition costs the
    host a few tens of MB, and FilePages.written() lets the store skip the holes."""
    with open(path, "wb") as f:
        f.truncate(size // 4096 * 4096)
    r = subprocess.run(["hdiutil", "attach", "-imagekey", "diskimage-class=CRawDiskImage", "-nomount", path],
                       check=True, capture_output=True, text=True)
    dev = r.stdout.split()[0]
    try:
        bs = ["-b", str(block_size)] if block_size else []
        subprocess.run(["newfs_hfs", "-s", *(["-J"] if journaled else []), *bs, "-v", "Data", dev], check=True,
                       capture_output=True)
    finally:
        subprocess.run(["hdiutil", "detach", dev], capture_output=True)
    return path


class FilePages:
    def __init__(self, path, page, patch=None):
        self.f = open(path, "rb")
        self.size = os.fstat(self.f.fileno()).st_size
        self.page, self.patch = page, patch or {}
        self.pages = -(-self.size // page)

    def written(self):
        """Page numbers inside the file's data extents (SEEK_DATA/SEEK_HOLE): a sparse image's holes are
        blocks nothing ever wrote, which HFS does not read before writing them, so they need no page."""
        fd, off, out = self.f.fileno(), 0, []
        while True:
            try:
                start = os.lseek(fd, off, os.SEEK_DATA)
            except OSError:
                break
            end = os.lseek(fd, start, os.SEEK_HOLE)
            out.extend(range(start // self.page, -(-end // self.page)))
            off = end
        return out

    def get(self, n):
        self.f.seek(n * self.page)
        d = self.f.read(self.page).ljust(self.page, b"\0")
        if n in self.patch:
            d = bytearray(d)
            for off, new in self.patch[n]:
                d[off:off + len(new)] = new
            d = bytes(d)
        return d


def find_fstab_patch(system_img, page):
    """in-place, same-length rewrite of /dev/disk0s2s1 -> /dev/disk0s2 in etc/fstab"""
    old, new = b"/dev/disk0s2s1 /private/var", b"/dev/disk0s2   /private/var"
    # every hit is rewritten: the live fstab plus any stale copies of it (the
    # captured volume has five, from repeated rewrites); a miss is harmless.
    patch = {}
    with open(system_img, "rb") as f:
        mm = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        off = mm.find(old)
        while off >= 0:
            pg, o = divmod(off, page)
            assert o + len(new) <= page
            patch.setdefault(pg, []).append((o, new))
            off = mm.find(old, off + 1)
        mm.close()
    print("      fstab: /dev/disk0s2s1 -> /dev/disk0s2 in %d system page(s)" % len(patch))
    return patch


# --- build -----------------------------------------------------------------

def build(a):
    geo = Geo(name=a.geometry, **GEOMETRIES[a.geometry])
    out = a.out
    if os.path.exists(os.path.join(out, "geometry.json")) and not a.force:
        raise SystemExit("%s exists; pass --force to overwrite" % out)
    if a.force and os.path.isdir(out):
        for n in os.listdir(out):
            if n == "geometry.json" or n.endswith(".pages"):
                os.unlink(os.path.join(out, n))
    ps = geo.page_size
    if ps != 4096:
        raise SystemExit("%s has %d-byte pages; the device MBR/partitions are 4 KiB-sectored" % (geo.name, ps))
    print("geometry %s: %d CS x %d blocks x %d pages x %d B; VFL usable %d/bank, YaFTL "
          "%d vblocks x %d pages, toc %d, exported %d sectors"
          % (geo.name, geo.num_cs, geo.blocks_per_ce, geo.pages_per_block, ps, geo.usable,
             geo.num_blocks, geo.ppsublk, geo.toc, geo.exported_pages))

    head = bytearray(open(a.mbr, "rb").read())
    if head[510:512] != b"\x55\xaa":
        raise SystemExit("%s: no MBR signature" % a.mbr)
    parts = mbr_parts(head)
    p1, p2, p3 = parts[0], parts[1], parts[2]
    system = FilePages(a.system, ps, find_fstab_patch(a.system, ps) if a.data != "none" else None)
    if system.pages != p1[2]:
        print("      warning: system image is %d pages, partition 1 is %d" % (system.pages, p1[2]))

    work = tempfile.mkdtemp(prefix="ipad1_nand.")
    data = None
    if a.data != "none":
        if os.path.exists(a.data):
            dpath = a.data
        else:
            print("      creating %s HFS+ data volume" % a.data)
            dpath = make_hfs_image(os.path.join(work, "data.dmg"), parse_size(a.data))
        data = FilePages(dpath, ps)
        # EncryptedMediaFilter (c04da880) refuses an 0xAE partition without a
        # 'tprc' key block unless nand-enable-reformat is set, so the data
        # partition becomes plain Apple_HFS (0xAF) at /dev/disk0s2 (fstab patched).
        set_part(head, 1, typ=0xAF, cnt=data.pages)
        p2 = (0xAF, p2[1], data.pages)
    if p2[1] + p2[2] > geo.exported_pages:
        raise SystemExit("partition 2 ends at %d > exported %d sectors" % (p2[1] + p2[2], geo.exported_pages))

    st = Store(out, geo, create=True)
    write_metadata(st, geo, kernel_version(a.kernelcache) if a.kernelcache else a.kernel_version,
                   nsig() if getattr(a, "epoch", None) or not a.kernelcache else 0x43313130 + nand_epoch(a.kernelcache))   # --epoch wins, else the kernel's
    ftl = FTLWriter(st, geo)
    # LPN == 4 KiB LBA. Segments in ascending LBA order:
    segs = [(0, min(p1[1], len(head) // ps), lambda n: bytes(head[n * ps:(n + 1) * ps]))]
    segs.append((p1[1], system.pages, system.get))
    if a.s3 and p3[2]:
        s3 = FilePages(a.s3, ps)
        segs.append((p3[1], min(p3[2], s3.pages), s3.get))
    segs = [(lba, range(count), get) for lba, count, get in segs]
    if data:
        written = data.written()
        print("      data partition: %d pages, %d written (the rest are holes)" % (data.pages, len(written)))
        segs.append((p2[1], written, data.get))
    total = sum(len(ns) for _, ns, _ in segs)
    done = 0
    for lba, pages, get in segs:
        for n in pages:
            ftl.user(lba + n, get(n))
            done += 1
            if done % 50000 == 0:
                print("      %d/%d pages" % (done, total))
    ftl.index_pages()
    st.close()
    shutil.rmtree(work, ignore_errors=True)
    print("wrote %s: %d pages (%.1f MiB), %d user/index vblocks used of %d, %d TOC pages"
          % (out, st.nrec, st.nrec * st.stride / 2 ** 20, ftl.vblock, geo.num_blocks, len(ftl.toc)))
    return out


# --- check -----------------------------------------------------------------

def find_special(st, geo, cs, magic):
    # _ReadSpecialBlock scan window (c07ffb60): blocks_per_ce-1 down to
    # blocks_per_ce - blocks_per_ce/10, page 0.. until the magic matches
    for blk in range(geo.blocks_per_ce - 1, geo.blocks_per_ce - geo.blocks_per_ce // 10 - 1, -1):
        d, _ = st.read(cs, geo.ppage(blk, 0))
        if d and d[:16] == magic.ljust(16, b"\0"):
            n = struct.unpack_from("<I", d, 0x34)[0]
            return blk, d[0x14:0x34], d[0x38:0x38 + n]
    return None, None, None


def check(path, mbr=None, system=None, geometry=None):
    fails = []

    def ok(cond, what):
        (print if cond else fails.append)(("ok   " if cond else "FAIL ") + what)

    if geometry is None:
        import json
        g = json.load(open(os.path.join(path, "geometry.json")))
        geometry = next((k for k, v in GEOMETRIES.items()
                         if (v["num_bus"], v["ce_per_bus"], v["blocks_per_ce"], v["pages_per_block"], v["page_size"])
                         == (g["buses"], g["ce_per_bus"], g["blocks_per_ce"], g["pages_per_block"], g["page_bytes"])), None)
        if not geometry:
            raise SystemExit("%s: no known geometry matches geometry.json" % path)
    geo = Geo(name=geometry, **GEOMETRIES[geometry])
    st = Store(path, geo)
    print("%s: %s" % (path, geometry))

    # special pages
    sig_block = None
    for cs in range(geo.num_cs):
        blk, cands, bbt = find_special(st, geo, cs, b"DEVICEINFOBBT")
        ok(blk is not None and len(bbt) == geo.bbt_len, "cs%d DEVICEINFOBBT at block 0x%x" % (cs, blk or 0))
        if blk is None:
            continue
        c = struct.unpack("<8I", cands)
        ok(not bbt[0] & 1 and all(not bbt[b // 8] >> (b % 8) & 1 for b in geo.cand[cs]),
           "cs%d BBT marks block 0 and candidates bad" % cs)
        ok(bbt[geo.vfl_blocks[0] // 8] >> (geo.vfl_blocks[0] % 8) & 1, "cs%d BBT block 1 good" % cs)
        d2, _ = st.read(cs, geo.ppage(c[1], 0))
        ok(d2 is not None and d2[:16] == b"DEVICEINFOBBT".ljust(16, b"\0"), "cs%d second BBT copy at 0x%x" % (cs, c[1]))
        if cs == 0:
            sig_block = c[4]
    d, _ = st.read(0, geo.ppage(sig_block or 0, 0))
    ok(d is not None and d[:16] == b"NANDDRIVERSIGN".ljust(16, b"\0"), "NANDDRIVERSIGN at cs0 block 0x%x (BBT hdr+0x24)" % (sig_block or 0))
    if d:
        nsig, flags = struct.unpack_from("<II", d, 0x38)
        ok(nsig & ~0xff == 0x43313100 and 0x31 <= nsig & 0xff <= 0x39 and flags == sig_flags(),
           "signature nSig=%08x flags=%08x (VSVFL, epoch %d, whitening %s)" % (nsig, flags, (nsig & 0xff) - 0x30,
                                                                            "on" if WHITENING else "off"))

    # VFL contexts
    for cs in range(geo.num_cs):
        copies = [st.read(cs, geo.ppage(geo.vfl_blocks[0], p)) for p in range(8)]
        ok(all(c[0] is not None and c[0][:0x800] == copies[0][0][:0x800] for c in copies), "cs%d VFLCxt 8 identical copies" % cs)
        ctx = bytearray(copies[0][0][:0x800])
        ok(bytes(ctx) == vfl_checksum(bytearray(ctx)), "cs%d VFLCxt checksums" % cs)
        ok(all(m[8] == 0 and m[9] == T_VFL for _, m in copies), "cs%d VFLCxt spare type 0x80" % cs)
        ver, ftl_type = struct.unpack_from("<I", ctx, 0x7f4)[0], struct.unpack_from("<I", ctx, 8)[0]
        usable, pstart, cb = struct.unpack_from("<HH3H", ctx, 0x696)[0], struct.unpack_from("<H", ctx, 0x698)[0], struct.unpack_from("<3H", ctx, 0x69a)
        ok(ver <= 2 and ftl_type == 2 and usable == geo.usable == pstart and list(cb) == geo.ctrl_blocks,
           "cs%d VFLCxt version %d ftl_type %d usable %d ctrl %s" % (cs, ver, ftl_type, usable, list(cb)))
        pool = struct.unpack_from("<%dH" % (geo.vfl_banks * geo.pool), ctx, 0x26)
        ok(all(pool[b * geo.pool + s] == pbn for pbn, (b, s) in geo.remap.items()), "cs%d pool map remaps blocks %s" % (cs, sorted(geo.remap)))

    # FTL walk: every vblock's page 0 classifies it (restore c0805ab4-style)
    toc_from_index, toc_from_user = {}, {}
    nuser = nindex = nfree = 0
    for v in range(geo.num_blocks):
        base = v * geo.ppsublk
        d, m = st.read_vpn(base)
        if d is None:
            nfree += 1
            continue
        typ = m[9]
        if v in geo.ctrl_blocks:
            fails.append("FAIL ctrl vblock %d is programmed" % v)
        if typ == T_INDEX:
            nindex += 1
        elif typ == T_USER:
            nuser += 1
        else:
            fails.append("FAIL vblock %d page 0 has spare type 0x%x" % (v, typ))
            continue
        # walk pages until clean; verify BTOC when the block is closed
        lpns = []
        for j in range(geo.ppsublk):
            pd, pm = st.read_vpn(base + j)
            if pd is None:
                break
            if pm[9] & T_CLOSED:
                break
            lpn = struct.unpack_from("<I", pm)[0]
            lpns.append(lpn)
            if typ == T_INDEX:
                toc_from_index[lpn] = array.array("I", pd)
            else:
                toc_from_user[lpn] = base + j
        if len(lpns) == geo.data_pages:
            table = b"".join(st.read_vpn(base + geo.data_pages + i)[0] for i in range(geo.toc))
            got = struct.unpack_from("<%dI" % geo.data_pages, table)
            ok(list(got) == lpns, "vblock %d closed, BTOC matches %d page spares" % (v, len(lpns))) if v % 25 == 0 or list(got) != lpns else None
    print("ok    %d user, %d index, %d free vblocks" % (nuser, nindex, nfree))
    rebuilt = {}
    for t, arr in toc_from_index.items():
        for i, vpn in enumerate(arr):
            if vpn != UNMAPPED:
                rebuilt[t * geo.toc_entries + i] = vpn
    ok(rebuilt == toc_from_user, "index pages map exactly the %d user pages" % len(toc_from_user))

    def lpn_data(lpn):
        return st.read_vpn(rebuilt[lpn])[0] if lpn in rebuilt else None

    m0 = lpn_data(0)
    ok(m0 is not None and m0[510:512] == b"\x55\xaa", "LBA 0 carries an MBR")
    if m0:
        parts = mbr_parts(m0)
        for i, (typ, lba, cnt) in enumerate(parts):
            if not typ:
                continue
            ok(lba + cnt <= geo.exported_pages, "partition %d type %02x lba %d count %d inside exported size" % (i + 1, typ, lba, cnt))
            if typ == 0xAF and cnt > 256:      # the real device's 32 KiB p3 stub is not a volume
                vh = lpn_data(lba)
                ok(vh is not None and vh[1024:1026] in (b"H+", b"HX"), "partition %d has an HFS+ volume header" % (i + 1))
        if mbr:
            src = open(mbr, "rb").read(512)
            ok(m0[:0x1be] == src[:0x1be] and parts[0] == mbr_parts(src)[0], "LBA 0 matches %s (boot code + partition 1)" % os.path.basename(mbr))
        if system:
            lba, cnt = parts[0][1], parts[0][2]
            f = open(system, "rb")
            bad = 0
            sample = list(range(64)) + list(range(0, cnt, max(1, cnt // 512)))
            for n in sample:
                f.seek(n * geo.page_size)
                want = f.read(geo.page_size).ljust(geo.page_size, b"\0")
                got = lpn_data(lba + n)
                if got != want and b"/dev/disk0s2" not in (got or b""):
                    bad += 1
            ok(bad == 0, "%d sampled system pages match %s" % (len(sample), os.path.basename(system)))
    print("\n".join(fails))
    print("%s: %s" % (path, "FAILED (%d)" % len(fails) if fails else "all checks passed"))
    return not fails


# --- selfcheck ---------------------------------------------------------------

def selfcheck():
    import random
    work = tempfile.mkdtemp(prefix="ipad1_nand_self.")
    ps = 4096
    rnd = random.Random(1)
    mbr = bytearray(ps * 63)
    mbr[510:512] = b"\x55\xaa"
    for i, (typ, lba, cnt) in enumerate([(0xAF, 63, 700), (0xAE, 800, 4000), (0xAF, 763, 8)]):
        struct.pack_into("<BBHBBHII", mbr, 0x1be + 16 * i, 0, 0, 0, typ, 0, 0, lba, cnt)
    sysimg = bytearray(rnd.getrandbits(8) for _ in range(ps * 700))
    sysimg[1024:1026] = b"HX"
    fst = ps * 300
    sysimg[fst:fst + 60] = b"/dev/disk0s1 / hfs rw 0 1\n/dev/disk0s2s1 /private/var hfs rw 0 2\n".ljust(60, b"\0")
    data = bytearray(ps * 500)
    data[1024:1026] = b"H+"
    paths = {}
    for name, blob in (("mbr", mbr), ("system.img", sysimg), ("data.img", data)):
        paths[name] = os.path.join(work, name)
        open(paths[name], "wb").write(blob)
    out = build(argparse.Namespace(geometry="selfcheck", mbr=paths["mbr"], system=paths["system.img"],
                                   s3=None, data=paths["data.img"], out=os.path.join(work, "nand"), force=True,
                                   kernelcache=None, kernel_version=b"Darwin Kernel Version selfcheck"))
    good = check(out, mbr=paths["mbr"], system=paths["system.img"])
    # the fstab rewrite must have landed
    geo = Geo(name="selfcheck", **GEOMETRIES["selfcheck"])
    st = Store(out, geo)
    ftl_page = None
    for v in range(3, geo.num_blocks):
        for j in range(geo.data_pages):
            d, m = st.read_vpn(v * geo.ppsublk + j)
            if d is None:
                break
            if m[9] == T_USER and struct.unpack_from("<I", m)[0] == 63 + 300:
                ftl_page = d
    good &= ftl_page is not None and b"/dev/disk0s2   /private/var" in ftl_page
    print("fstab patched:", ftl_page is not None and b"/dev/disk0s2   /private/var" in ftl_page)
    shutil.rmtree(work, ignore_errors=True)
    # make_mbr reproduces the 16 GB unit's sector 0 (checked byte for byte against its rdisk0 dump)
    head = make_mbr(Geo(name="k48-16g", **GEOMETRIES["k48-16g"]), 1280)
    unit = bytes.fromhex("00010100affeffff3f00000000000500" "00feffffaefeffff6d0005002fe53600"
                         "00feffffaffeffff4000050008000000")
    mbr_ok = head[0x1be:0x1ee] == unit and head[510:512] == b"\x55\xaa" and not any(head[:0x1be] + head[0x1ee:510] + head[512:])
    print("make_mbr matches the unit's sector 0:", mbr_ok)
    good &= mbr_ok
    print("selfcheck", "passed" if good else "FAILED")
    return good


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--selfcheck", action="store_true")
    sub = ap.add_subparsers(dest="cmd")
    b = sub.add_parser("build")
    b.add_argument("--geometry", default="k48-16g", choices=[k for k in GEOMETRIES if k != "selfcheck"])
    b.add_argument("--oracle", default=None, help=argparse.SUPPRESS)
    b.add_argument("--mbr", required=True, help="first sectors of the logical device (`mbr` output)")
    b.add_argument("--kernelcache", required=True, help="decrypted kernelcache (ipad1_fw.py), for NANDDRIVERSIGN's version string")
    b.add_argument("--system", required=True, help="raw system partition image (rdisk0s1)")
    b.add_argument("--s3", default=None, help="raw partition 3 image (rdisk0s3.bin), optional")
    b.add_argument("--data", default="1g", help="data partition: SIZE (fresh HFS+ via hdiutil), IMAGE, or none")
    b.add_argument("--out", required=True)
    b.add_argument("--force", action="store_true")
    b.add_argument("--no-whitening", action="store_true", help="plain meta, signature flags 0x5 (DT without metadata-whitening)")
    b.add_argument("--epoch", type=int, default=None, help="NAND epoch, the IPSW's Restore.plist DeviceMap SCEP (default: the kernel's PE_nand_epoch)")
    b.add_argument("--sig-flags", type=lambda v: int(v, 0), default=None,
                   help="NANDDRIVERSIGN flags as the build's driver formats them (3.1.x: 4; default 0x10005/0x5)")
    m = sub.add_parser("mbr")
    m.add_argument("--geometry", default="k48-16g", choices=[k for k in GEOMETRIES if k != "selfcheck"])
    m.add_argument("--system-mib", type=int, default=1280)
    m.add_argument("out")
    c = sub.add_parser("check")
    c.add_argument("dir")
    c.add_argument("--no-whitening", action="store_true")
    c.add_argument("--mbr")
    c.add_argument("--system")
    a = ap.parse_args()
    global WHITENING, EPOCH, SIG_FLAGS_OVERRIDE
    WHITENING = not getattr(a, "no_whitening", False)
    SIG_FLAGS_OVERRIDE = getattr(a, "sig_flags", None)
    EPOCH = getattr(a, "epoch", None) or 1
    if a.selfcheck:
        sys.exit(0 if selfcheck() else 1)
    if a.cmd == "build":
        build(a)
    elif a.cmd == "mbr":
        with open(a.out, "wb") as f:
            f.write(make_mbr(Geo(name=a.geometry, **GEOMETRIES[a.geometry]), a.system_mib))
    elif a.cmd == "check":
        sys.exit(0 if check(a.dir, a.mbr, a.system) else 1)
    else:
        ap.print_help()


if __name__ == "__main__":
    main()
