#!/usr/bin/env python3
"""Turn a 3.x-era dyld shared cache back into one Mach-O per library, for lldb's userland symbols.

    dsc_extract.py dyld_shared_cache_armv6 OUTROOT [NAME-SUBSTRING ...]

Each image is written to OUTROOT/<its install path>. Its own segments are copied, and a private
__LINKEDIT is built from its symbol table in the cache's shared one. Addresses stay the cache's. Then
`xnu-images --sysroot OUTROOT` (xnu.py) adds them to lldb as it does 1.x's plain files. That also needs
OUTROOT/usr/lib/dyld from the same rootfs, because dyld_all_image_infos is found through it.

Format: the original `dyld_v1` cache (iPhone OS 3.x, dyld-95/132; header, mappings and images as in
dyld's dyld_cache_format.h of that era). ipsw and modern tools no longer parse it. Only symbols and
code are kept. The stubs' indirect tables and the dyld info are dropped, so lldb names functions but
not stub targets.
"""
import os
import struct
import sys

LC_SEGMENT, LC_SYMTAB, LC_DYSYMTAB = 0x1, 0x2, 0xB
LINKEDIT_DATA = (0x1D, 0x1E, 0x26, 0x29)     # code signature, split info, function starts, data in code


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    cache, out, want = sys.argv[1], sys.argv[2], sys.argv[3:]
    b = open(cache, "rb").read()
    if not b.startswith(b"dyld_v1"):
        sys.exit("%s: not a dyld_v1 shared cache" % cache)
    map_off, map_n, img_off, img_n = struct.unpack_from("<IIII", b, 16)
    maps = [struct.unpack_from("<QQQ", b, map_off + 32 * i) for i in range(map_n)]   # address, size, fileoff

    def off(va):
        for a, size, fo in maps:
            if a <= va < a + size:
                return fo + va - a
        raise ValueError("0x%x is not in the cache" % va)

    done = 0
    for i in range(img_n):
        addr, _mtime, _inode, path_off = struct.unpack_from("<QQQI", b, img_off + 32 * i)
        path = b[path_off:b.index(b"\0", path_off)].decode()
        if want and not any(w in path for w in want):
            continue
        dest = os.path.join(out, path.lstrip("/"))
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        with open(dest, "wb") as f:
            f.write(extract(b, off(addr), off))
        done += 1
    print("%d images written under %s" % (done, out))


def extract(b, h, off):
    magic, cpu, sub, ftype, ncmds, sizeofcmds, flags = struct.unpack_from("<7I", b, h)
    cmds = bytearray(b[h + 28:h + 28 + sizeofcmds])
    segs, symtab = [], None
    o = 0
    for _ in range(ncmds):
        cmd, size = struct.unpack_from("<II", cmds, o)
        if cmd == LC_SEGMENT:
            segs.append(o)
        elif cmd == LC_SYMTAB:
            symtab = o
        o += size

    body, pos = bytearray(), 0
    for so in segs:                                   # segments in order, page-aligned, linkedit last
        name = cmds[so + 8:so + 24].split(b"\0")[0]
        vm, vms, fo, fs = struct.unpack_from("<IIII", cmds, so + 24)
        if name == b"__LINKEDIT":
            continue
        pos = (len(body) + 0xFFF) & ~0xFFF
        body.extend(bytes(pos - len(body)))
        src = off(vm)                                 # the cache's file offsets are its own, not the image's
        body.extend(b[src:src + fs])
        struct.pack_into("<I", cmds, so + 32, pos)
        nsect = struct.unpack_from("<I", cmds, so + 48)[0]
        for s in range(nsect):                        # section offsets follow their segment (not zerofill)
            sp = so + 56 + 68 * s
            saddr = struct.unpack_from("<I", cmds, sp + 32)[0]
            if struct.unpack_from("<I", cmds, sp + 40)[0]:
                struct.pack_into("<I", cmds, sp + 40, saddr - vm + pos)

    # a private __LINKEDIT: this image's nlists, renumbered into its own string table
    link = bytearray()
    if symtab is not None:
        symoff, nsyms, stroff, strsize = struct.unpack_from("<IIII", cmds, symtab + 8)
        strings, index = bytearray(b"\0"), {}
        nl = bytearray()
        for k in range(nsyms):
            strx, typ, sect, desc, val = struct.unpack_from("<IBBHI", b, symoff + 12 * k)
            name = b[stroff + strx:b.index(b"\0", stroff + strx)]
            if name not in index:
                index[name] = len(strings)
                strings += name + b"\0"
            nl += struct.pack("<IBBHI", index[name], typ, sect, desc, val)
        link_pos = (len(body) + 0xFFF) & ~0xFFF
        struct.pack_into("<IIII", cmds, symtab + 8, link_pos, nsyms, link_pos + len(nl), len(strings))
        link = nl + strings
    else:
        link_pos = (len(body) + 0xFFF) & ~0xFFF

    o = 0
    for _ in range(ncmds):
        cmd, size = struct.unpack_from("<II", cmds, o)
        if cmd == LC_SEGMENT and cmds[o + 8:o + 24].split(b"\0")[0] == b"__LINKEDIT":
            struct.pack_into("<IIII", cmds, o + 24, struct.unpack_from("<I", cmds, o + 24)[0],
                             (len(link) + 0xFFF) & ~0xFFF, link_pos, len(link))
        elif cmd == LC_DYSYMTAB:                      # keep the symbol ranges, drop the shared tables
            vals = list(struct.unpack_from("<18I", cmds, o + 8))
            for k in range(6, 18):
                vals[k] = 0
            struct.pack_into("<18I", cmds, o + 8, *vals)
        elif cmd in LINKEDIT_DATA:
            struct.pack_into("<II", cmds, o + 8, 0, 0)
        o += size

    body[28:28 + sizeofcmds] = cmds                   # the header lives at the start of __TEXT
    # MH_DYLIB_IN_CACHE: lldb would read this image's linkedit as a cache's; it is a file of its own now
    struct.pack_into("<I", body, 24, flags & ~0x80000000)
    body.extend(bytes(link_pos - len(body)))
    return bytes(body + link)


if __name__ == "__main__":
    main()
