#!/usr/bin/env python3
"""Write a .tbd link stub for a dylib or framework taken from a guest's own root filesystem.

    mktbd.py ROOT INSTALL-PATH OUT.tbd      e.g. mktbd.py rootfs /System/Library/Frameworks/UIKit.framework/UIKit UIKit.tbd

iPhone OS 1.x had no SDK, so its frameworks themselves are the link target: the stub carries the binary's
install name, versions and exported symbols (fragile-ABI classes as their `.objc_class_name_X` symbols,
which is how that ABI references them), declared for armv7, the arch link6 links as. Modern ld/nm refuse
the 1.x binaries outright (LC_PREBIND_CKSUM), so the symbol table is read here.
"""
import os
import struct
import sys

LC_SYMTAB, LC_ID_DYLIB = 0x2, 0xD
N_EXT, N_TYPE, N_SECT, N_ABS = 0x01, 0x0E, 0x0E, 0x02


def read(path):
    b = open(path, "rb").read()
    if struct.unpack_from("<I", b, 0)[0] == 0xCAFEBABE:      # fat: the first arm slice
        n = struct.unpack_from(">I", b, 4)[0]
        for i in range(n):
            cpu, _, off, size, _ = struct.unpack_from(">IIIII", b, 8 + 20 * i)
            if cpu == 12:
                b = b[off:off + size]
                break
    magic, cpu, sub, ftype, ncmds = struct.unpack_from("<IIIII", b, 0)
    if magic != 0xFEEDFACE or cpu != 12:
        sys.exit("%s: not a 32-bit ARM Mach-O" % path)
    off, syms, ident = 28, [], None
    for _ in range(ncmds):
        cmd, size = struct.unpack_from("<II", b, off)
        if cmd == LC_ID_DYLIB:
            name_off, _ts, cur, compat = struct.unpack_from("<IIII", b, off + 8)
            ident = (b[off + name_off:off + size].split(b"\0")[0].decode(), cur, compat)
        elif cmd == LC_SYMTAB:
            symoff, nsyms, stroff, _ = struct.unpack_from("<IIII", b, off + 8)
            for i in range(nsyms):
                strx, typ, _sect, _desc, _val = struct.unpack_from("<IBBHI", b, symoff + 12 * i)
                if typ & N_EXT and typ & N_TYPE in (N_SECT, N_ABS):
                    syms.append(b[stroff + strx:b.index(b"\0", stroff + strx)].decode("latin1"))
        off += size
    if not ident:
        sys.exit("%s: no LC_ID_DYLIB" % path)
    return ident, sorted(set(syms))


def version(v):
    return "%d.%d.%d" % (v >> 16, (v >> 8) & 0xFF, v & 0xFF)


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    root, install, out = sys.argv[1:]
    (name, cur, compat), syms = read(os.path.join(root, install.lstrip("/")))
    quoted = ",\n                 ".join("'%s'" % s for s in syms)
    with open(out, "w") as f:
        f.write("--- !tapi-tbd\ntbd-version: 4\ntargets: [ armv7-ios ]\ninstall-name: '%s'\n"
                "current-version: %s\ncompatibility-version: %s\nexports:\n  - targets: [ armv7-ios ]\n"
                "    symbols: [ %s ]\n...\n" % (name, version(cur), version(compat), quoted))
    print("%s: %d symbols" % (out, len(syms)))


if __name__ == "__main__":
    main()
