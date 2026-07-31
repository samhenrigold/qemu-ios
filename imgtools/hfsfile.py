#!/usr/bin/env python3
"""Locate and patch HFS+/HFSX file records inside the raw NAND page image.

The emulator serves NAND pages straight out of `cs{0..3}/<page>.page` with no
ECC or spare-area validation (`fmss_load_page` in hw/arm/ipod_touch_fmss.c), so
a page file edited on the host is exactly what the guest's FTL will read back.
That lets us edit the guest filesystem without understanding the FTL: find the
bytes by content, rewrite them in place.

Two things have to move together to change a file's contents:

  1. the allocation block holding the data (found by searching for the current
     content), and
  2. the catalog record's `dataFork.logicalSize`, if the length changes.

Growing a file *within* its already-allocated block needs nothing else - no
allocation-bitmap edit, no extent change. The volume is unjournaled (attributes
0x80000100, journalInfoBlock 0) so there is no log to replay over our edit.
"""

import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nandblob as nb

# HFSPlusCatalogFile field offsets, from the record start.
REC_TYPE = 0          # SInt16, 2 == kHFSPlusFileRecord
REC_FLAGS = 2         # UInt16
REC_RESERVED1 = 4     # UInt32
REC_FILEID = 8        # UInt32
REC_PERMISSIONS = 32  # HFSPlusBSDInfo, 16 bytes
REC_DATAFORK = 88     # HFSPlusForkData
FORK_LOGICAL = 0      # UInt64
FORK_CLUMP = 8        # UInt32
FORK_TOTALBLOCKS = 12 # UInt32
FORK_EXTENTS = 16     # 8 x (startBlock UInt32, blockCount UInt32)

kHFSPlusFileRecord = 0x0002


def decode_record(page, name_off, name):
    """Decode a catalog file record whose UTF-16BE name starts at name_off.

    Returns None if the surrounding bytes are not a well-formed catalog key +
    file record (the same byte sequence turns up inside ordinary file data).
    """
    key = name_off - 8  # keyLength(2) parentID(4) nodeName.length(2)
    if key < 0:
        return None
    key_len, parent_id, name_len = struct.unpack_from(">HIH", page, key)
    if name_len != len(name) or key_len != 4 + 2 + 2 * name_len:
        return None
    rec = key + 2 + key_len
    if rec % 2:
        rec += 1
    if rec + 248 > len(page):
        return None
    rtype, flags, _res, file_id = struct.unpack_from(">HHII", page, rec)
    if rtype != kHFSPlusFileRecord:
        return None
    logical, clump, total = struct.unpack_from(">QII", page, rec + REC_DATAFORK)
    extents = [
        struct.unpack_from(">II", page, rec + REC_DATAFORK + FORK_EXTENTS + i * 8)
        for i in range(8)
    ]
    # HFSPlusBSDInfo: ownerID(4) groupID(4) adminFlags(1) ownerFlags(1) fileMode(2)
    mode = struct.unpack_from(">H", page, rec + REC_PERMISSIONS + 10)[0]
    return {
        "key_off": key,
        "rec_off": rec,
        "parent_id": parent_id,
        "file_id": file_id,
        "flags": flags,
        "mode": mode,
        "logical_size": logical,
        "clump": clump,
        "total_blocks": total,
        "extents": [e for e in extents if e[1]],
    }


def find_catalog_records(blob, name):
    """All plausible catalog file records for `name`, as (cs, page, info)."""
    hits = nb.search(blob, name.encode("utf-16-be"), limit=200)
    out = []
    for _off, cs, pg, o in hits:
        path = nb.page_path(nb_nand_of(blob), cs, pg)
        with open(path, "rb") as f:
            page = f.read(nb.DATA)
        info = decode_record(page, o, name)
        if info:
            info["cs"], info["page"] = cs, pg
            out.append(info)
    return out


_NAND_OF = {}


def nb_nand_of(blob):
    return _NAND_OF[blob]


def register(blob, nand):
    _NAND_OF[blob] = nand


def find_content_page(blob, prefix):
    """Pages whose data area begins with (or contains) `prefix`."""
    return nb.search(blob, prefix, limit=50)


def write_page_data(nand, cs, page, data):
    """Replace the 4096-byte data area of one page file, keeping its spare."""
    path = nb.page_path(nand, cs, page)
    with open(path, "rb") as f:
        buf = bytearray(f.read())
    if len(buf) != nb.PAGE:
        raise RuntimeError("unexpected page size %d for %s" % (len(buf), path))
    if len(data) > nb.DATA:
        raise ValueError("data too long for one page")
    buf[0 : nb.DATA] = data.ljust(nb.DATA, b"\x00")
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(buf)
    os.replace(tmp, path)


def set_logical_size(nand, cs, page, rec_off, new_size):
    path = nb.page_path(nand, cs, page)
    with open(path, "rb") as f:
        buf = bytearray(f.read())
    struct.pack_into(">Q", buf, rec_off + REC_DATAFORK + FORK_LOGICAL, new_size)
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(buf)
    os.replace(tmp, path)
