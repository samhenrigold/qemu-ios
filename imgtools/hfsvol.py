#!/usr/bin/env python3
"""A read/write HFS+ volume backed by the raw NAND page files.

Allocation blocks are addressed through the closed-form layout formula in
ftlmap.py, so this behaves like an ordinary block device: block N is at
`cs<cs>/<page>.page`, and a block that has never been written simply has no page
file (the emulator serves those as blank, so creating the file is how you write
into free space).

Provides the volume header, the allocation bitmap, and enough B-tree machinery
to read the catalog.
"""

import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nandblob as nb
from ftlmap import predict

BLOCK = 4096

# B-tree node kinds
kBTLeafNode = -1
kBTIndexNode = 0
kBTHeaderNode = 1
kBTMapNode = 2

# catalog record types
kHFSPlusFolderRecord = 0x0001
kHFSPlusFileRecord = 0x0002
kHFSPlusFolderThreadRecord = 0x0003
kHFSPlusFileThreadRecord = 0x0004


class Volume:
    def __init__(self, nand, writable=False):
        self.nand = nand
        self.writable = writable
        self.dirty = set()
        self._cache = {}
        self.vh = self.read_block(0)[1024:1536]
        sig, ver = struct.unpack_from(">2sH", self.vh, 0)
        if sig not in (b"H+", b"HX"):
            raise RuntimeError("no HFS+ volume header (got %r)" % sig)
        self.signature = sig
        self.block_size = struct.unpack_from(">I", self.vh, 40)[0]
        self.total_blocks = struct.unpack_from(">I", self.vh, 44)[0]
        self.free_blocks = struct.unpack_from(">I", self.vh, 48)[0]
        self.next_catalog_id = struct.unpack_from(">I", self.vh, 64)[0]
        self.file_count = struct.unpack_from(">I", self.vh, 32)[0]
        self.folder_count = struct.unpack_from(">I", self.vh, 36)[0]
        if self.block_size != BLOCK:
            raise RuntimeError("unexpected block size %d" % self.block_size)
        self.allocation = Fork(self, self.vh, 112)
        self.extents = Fork(self, self.vh, 192)
        self.catalog = Fork(self, self.vh, 272)
        self.attributes = Fork(self, self.vh, 352)

    # --- raw block access -------------------------------------------------
    def block_path(self, n):
        cs, pg = predict(n)
        return nb.page_path(self.nand, cs, pg)

    def read_block(self, n):
        if n in self._cache:
            return self._cache[n]
        p = self.block_path(n)
        try:
            with open(p, "rb") as f:
                data = bytearray(f.read(BLOCK).ljust(BLOCK, b"\x00"))
        except FileNotFoundError:
            data = bytearray(BLOCK)
        self._cache[n] = data
        return data

    def write_block(self, n, data):
        if not self.writable:
            raise RuntimeError("volume opened read-only")
        if len(data) != BLOCK:
            raise ValueError("block must be %d bytes" % BLOCK)
        self._cache[n] = bytearray(data)
        self.dirty.add(n)

    def flush(self):
        """Write dirty blocks back, preserving each page's 64-byte spare."""
        for n in sorted(self.dirty):
            p = self.block_path(n)
            spare = b"\x00" * 8 + struct.pack("<I", 0x00FF00FF) + b"\x00" * 52
            if os.path.exists(p):
                with open(p, "rb") as f:
                    old = f.read()
                if len(old) >= nb.PAGE:
                    spare = old[nb.DATA : nb.PAGE]
            else:
                os.makedirs(os.path.dirname(p), exist_ok=True)
            tmp = p + ".tmp"
            with open(tmp, "wb") as f:
                f.write(bytes(self._cache[n]))
                f.write(spare)
            os.replace(tmp, p)
        n = len(self.dirty)
        self.dirty.clear()
        return n

    def sync_header(self):
        """Write the in-memory counters back to both volume headers."""
        struct.pack_into(">I", self.vh, 32, self.file_count)
        struct.pack_into(">I", self.vh, 36, self.folder_count)
        struct.pack_into(">I", self.vh, 48, self.free_blocks)
        struct.pack_into(">I", self.vh, 64, self.next_catalog_id)
        b0 = self.read_block(0)
        b0[1024:1536] = self.vh
        self.write_block(0, b0)
        last = self.read_block(self.total_blocks - 1)
        last[BLOCK - 1024 : BLOCK - 512] = self.vh
        self.write_block(self.total_blocks - 1, last)


class Fork:
    """A special-file fork described by an HFSPlusForkData at vh[off:off+80]."""

    def __init__(self, vol, buf, off):
        self.vol = vol
        self.logical_size, self.clump, self.total_blocks = struct.unpack_from(
            ">QII", buf, off)
        self.extents = [struct.unpack_from(">II", buf, off + 16 + i * 8)
                        for i in range(8)]
        self.extents = [e for e in self.extents if e[1]]

    def blocks(self):
        out = []
        for start, count in self.extents:
            out.extend(range(start, start + count))
        return out

    def read(self, offset, length):
        blks = self.blocks()
        out = bytearray()
        while length > 0:
            i, o = divmod(offset, BLOCK)
            if i >= len(blks):
                break
            chunk = self.vol.read_block(blks[i])[o : o + length]
            out += chunk
            offset += len(chunk)
            length -= len(chunk)
        return bytes(out)

    def write(self, offset, data):
        blks = self.blocks()
        while data:
            i, o = divmod(offset, BLOCK)
            if i >= len(blks):
                raise RuntimeError("write past end of fork")
            b = self.vol.read_block(blks[i])
            n = min(len(data), BLOCK - o)
            b[o : o + n] = data[:n]
            self.vol.write_block(blks[i], b)
            data = data[n:]
            offset += n


class BTree:
    def __init__(self, fork):
        self.fork = fork
        hdr = fork.read(0, 512)
        # node descriptor is the first 14 bytes; BTHeaderRec follows
        (self.tree_depth, self.root, self.leaf_records, self.first_leaf,
         self.last_leaf, self.node_size, self.max_key_len, self.total_nodes,
         self.free_nodes) = struct.unpack_from(">HIIIIHHII", hdr, 14)
        self.attributes = struct.unpack_from(">I", hdr, 14 + 32 + 8)[0]

    def node(self, n):
        return self.fork.read(n * self.node_size, self.node_size)

    def node_info(self, buf):
        flink, blink, kind, height, num_recs = struct.unpack_from(">IIbBH", buf, 0)
        offs = [struct.unpack_from(">H", buf, self.node_size - 2 * (i + 1))[0]
                for i in range(num_recs + 1)]
        return flink, blink, kind, height, num_recs, offs

    def leaf_nodes(self):
        n = self.first_leaf
        while n:
            buf = self.node(n)
            yield n, buf
            n = self.node_info(buf)[0]

    def free_space(self, buf):
        _f, _b, _k, _h, num_recs, offs = self.node_info(buf)
        used_end = offs[num_recs - 1] if num_recs else 14
        # records grow up from 14, the offset array grows down from node_size
        return self.node_size - 2 * (num_recs + 1) - offs[num_recs]

    def records(self, buf):
        _f, _b, _k, _h, num_recs, offs = self.node_info(buf)
        # offsets are stored in reverse: offs[0] is record 0
        out = []
        for i in range(num_recs):
            out.append(buf[offs[i] : offs[i + 1]] if i + 1 <= num_recs else b"")
        return out


def cat_key(rec):
    """(parentID, name) from a catalog key at the start of `rec`."""
    key_len, parent, name_len = struct.unpack_from(">HIH", rec, 0)
    name = rec[8 : 8 + 2 * name_len].decode("utf-16-be", "replace")
    body = 2 + key_len
    if body % 2:
        body += 1
    return parent, name, body
