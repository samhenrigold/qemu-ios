#!/usr/bin/env python3
"""objc1dump.py BINARY [CLASS...] -- class-dump for the fragile (objc1) ABI of iPhone OS 1.x binaries.

Reads the __OBJC segment of a 32-bit little-endian Mach-O (module info -> symtab -> class definitions) and
prints each class as an @interface: superclass, instance size, ivars with offsets, instance (-) and class (+)
methods with their type encodings. With CLASS arguments, only those classes. How the kitchen sink's
interfaces (uikit1.h) were discovered: from 1.0's own UIKit, not a later SDK.
"""
import struct, sys


class MachO:
    def __init__(self, path):
        d = self.d = open(path, "rb").read()
        assert struct.unpack_from("<I", d)[0] == 0xFEEDFACE, "not a 32-bit LE Mach-O"
        ncmds = struct.unpack_from("<I", d, 16)[0]
        off, self.segs, self.sects = 28, [], {}
        for _ in range(ncmds):
            cmd, size = struct.unpack_from("<II", d, off)
            if cmd == 1:   # LC_SEGMENT
                seg = d[off + 8:off + 24].rstrip(b"\0").decode()
                va, vs, fo, fs, _, _, nsect, _ = struct.unpack_from("<IIIIIIII", d, off + 24)
                self.segs.append((va, vs, fo, fs))
                for i in range(nsect):
                    s = off + 56 + i * 68
                    name = d[s:s + 16].rstrip(b"\0").decode()
                    addr, ssize, sfo = struct.unpack_from("<III", d, s + 32)
                    self.sects[(seg, name)] = (addr, ssize, sfo)
            off += size

    def fo(self, va):
        for v, vs, f, fs in self.segs:
            if v <= va < v + fs:
                return f + va - v
        return None

    def u32(self, va):
        return struct.unpack_from("<I", self.d, self.fo(va))[0]

    def cstr(self, va):
        f = self.fo(va) if va else None
        return self.d[f:self.d.index(b"\0", f)].decode("latin1") if f is not None else None


def classes(m):
    addr, size, _ = m.sects[("__OBJC", "__module_info")]
    for mod in range(addr, addr + size, 16):
        symtab = m.u32(mod + 12)
        if not symtab or m.fo(symtab) is None:
            continue
        cls_n, cat_n = struct.unpack_from("<HH", m.d, m.fo(symtab + 8))
        for i in range(cls_n):
            yield m.u32(symtab + 12 + 4 * i)


def methods(m, cls):
    lst = m.u32(cls + 28)
    out = []
    if lst:
        n = m.u32(lst + 4)
        for i in range(n):
            e = lst + 8 + 12 * i
            out.append((m.cstr(m.u32(e)), m.cstr(m.u32(e + 4))))
    return out


def dump(m, cls):
    name, sup, size = m.cstr(m.u32(cls + 8)), m.cstr(m.u32(cls + 4)), m.u32(cls + 20)
    lines = ["@interface %s : %s   // instance size %d" % (name, sup or "(root)", size)]
    iv = m.u32(cls + 24)
    if iv:
        for i in range(m.u32(iv)):
            e = iv + 4 + 12 * i
            lines.append("    %-28s %-24s // +%d" % (m.cstr(m.u32(e)), m.cstr(m.u32(e + 4)), m.u32(e + 8)))
    for sel, types in methods(m, m.u32(cls)):   # the metaclass: class methods
        lines.append("+ %s   %s" % (sel, types))
    for sel, types in methods(m, cls):
        lines.append("- %s   %s" % (sel, types))
    return "\n".join(lines + ["@end"])


if __name__ == "__main__":
    m = MachO(sys.argv[1])
    want = set(sys.argv[2:])
    for c in classes(m):
        if not want or m.cstr(m.u32(c + 8)) in want:
            print(dump(m, c) + "\n")
