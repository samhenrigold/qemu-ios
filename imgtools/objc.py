#!/usr/bin/env python3
"""Map ObjC selectors to IMPs in a 32-bit ARM Mach-O (ObjC2 metadata)."""
import struct, sys
from macho import Macho

class O:
    def __init__(self, path):
        self.m = Macho(path); self.d = self.m.data
    def off(self, va): return self.m.va2off(va)
    def u32(self, va): return struct.unpack_from('<I', self.d, self.off(va))[0]
    def cstr(self, va):
        o = self.off(va)
        return self.d[o:self.d.find(b'\0', o)].decode('ascii', 'replace')
    def methods(self, mlist_va):
        if not mlist_va: return []
        entsize, count = struct.unpack_from('<2I', self.d, self.off(mlist_va))
        out = []
        for i in range(count):
            base = mlist_va + 8 + i * entsize
            name, types, imp = struct.unpack_from('<3I', self.d, self.off(base))
            out.append((self.cstr(name), imp))
        return out
    def classes(self, list_va, list_size):
        res = []
        for i in range(list_size // 4):
            cls = self.u32(list_va + i * 4)
            isa, sup, cache, vtable, data = struct.unpack_from('<5I', self.d, self.off(cls))
            ro = data & ~3
            flags, istart, isize, ivarlayout, name, methods = struct.unpack_from('<6I', self.d, self.off(ro))
            res.append((self.cstr(name), self.methods(methods)))
            # metaclass
            misa, msup, mc, mv, mdata = struct.unpack_from('<5I', self.d, self.off(isa))
            mro = mdata & ~3
            mf, mis, msz, miv, mname, mmeth = struct.unpack_from('<6I', self.d, self.off(mro))
            res.append(('+' + self.cstr(mname), self.methods(mmeth)))
        return res

if __name__ == '__main__':
    o = O(sys.argv[1])
    pat = sys.argv[2].lower() if len(sys.argv) > 2 else ''
    for cname, ms in o.classes(int(sys.argv[3], 16), int(sys.argv[4], 0)):
        for sel, imp in ms:
            if pat in sel.lower() or pat in cname.lower():
                print('%#010x  -[%s %s]' % (imp, cname, sel))
