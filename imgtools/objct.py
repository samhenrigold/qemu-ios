#!/usr/bin/env python3
"""Dump an ObjC class's ivars and method type encodings from a 32-bit ARM Mach-O.

objc.py maps selectors to IMPs, which is what you want when you are about to
patch one. This is for the other job: deciding whether a class can be built and
driven from outside, where the argument types and the ivar layout are the whole
question -- `q` is an int64, `i` an int, `c` a BOOL, `@"NSString"` an object.

    python3 objct.py <macho> <class-name-substring> <classlist-va> <classlist-size>

The classlist address and size come from `otool -l <macho>` under
(__DATA,__objc_classlist). Worked example, the class behind the App Store
progress bar (see contrib/it-instprogress/README.md):

    python3 objct.py iTunesStore_armv6 ISOperationProgress 0x48c50 0x14c
"""
import struct
import sys

from macho import Macho


class O:
    def __init__(self, path):
        self.m = Macho(path)
        self.d = self.m.data

    def off(self, va):
        return self.m.va2off(va)

    def u32(self, va):
        return struct.unpack_from('<I', self.d, self.off(va))[0]

    def cstr(self, va):
        o = self.off(va)
        return self.d[o:self.d.find(b'\0', o)].decode('ascii', 'replace')

    def methods(self, mlist_va):
        if not mlist_va:
            return []
        entsize, count = struct.unpack_from('<2I', self.d, self.off(mlist_va))
        out = []
        for i in range(count):
            base = mlist_va + 8 + i * entsize
            name, types, imp = struct.unpack_from('<3I', self.d, self.off(base))
            out.append((self.cstr(name), self.cstr(types), imp))
        return out

    def ivars(self, ivar_va):
        if not ivar_va:
            return []
        entsize, count = struct.unpack_from('<2I', self.d, self.off(ivar_va))
        out = []
        for i in range(count):
            b = ivar_va + 8 + i * entsize
            off_p, name, types, align, size = struct.unpack_from('<5I', self.d, self.off(b))
            # The entry holds a POINTER to the offset, not the offset itself:
            # the runtime rewrites it when a superclass changes size.
            out.append((self.cstr(name), self.cstr(types), self.u32(off_p), size))
        return out

    def classes(self, list_va, list_size):
        res = []
        for i in range(list_size // 4):
            cls = self.u32(list_va + i * 4)
            isa, sup, cache, vtable, data = struct.unpack_from('<5I', self.d, self.off(cls))
            ro = data & ~3
            # class_ro_t: flags, instanceStart, instanceSize, ivarLayout, name,
            #             baseMethods, baseProtocols, ivars, ...
            flags, istart, isize, ivarlayout, name, methods = \
                struct.unpack_from('<6I', self.d, self.off(ro))
            _protocols, ivars = struct.unpack_from('<2I', self.d, self.off(ro) + 24)
            res.append((self.cstr(name), self.methods(methods), self.ivars(ivars)))
        return res


if __name__ == '__main__':
    if len(sys.argv) < 5:
        sys.exit(__doc__)
    o = O(sys.argv[1])
    want = sys.argv[2].lower()
    for cname, ms, ivs in o.classes(int(sys.argv[3], 16), int(sys.argv[4], 0)):
        if want not in cname.lower():
            continue
        print('@interface %s' % cname)
        for n, t, off, sz in ivs:
            print('    ivar  %-28s %-20s +%#x (%d bytes)' % (n, t, off, sz))
        for sel, types, imp in sorted(ms):
            print('    %-46s %-20s %#010x' % (sel, types, imp))
        print()
