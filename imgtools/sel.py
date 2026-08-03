#!/usr/bin/env python3
import sys, struct
from macho import Macho
from capstone import Cs, CS_ARCH_ARM, CS_MODE_THUMB
def run(path, start, length):
    m = Macho(path); d = m.data
    def s_at(va):
        o = m.va2off(va)
        if o is None: return None
        s = d[o:d.find(b"\0", o)]
        if 0 < len(s) < 80 and all(32 <= c < 127 for c in s): return s.decode()
        return None
    md = Cs(CS_ARCH_ARM, CS_MODE_THUMB); pos = 0
    code = d[m.va2off(start):m.va2off(start)+length]
    while pos < len(code):
        got = False
        for i in md.disasm(code[pos:], start+pos):
            ann = ""
            if "pc" in i.op_str and i.mnemonic.startswith("ldr"):
                try:
                    disp = int(i.op_str.split("#")[-1].rstrip("]"), 0)
                    la = ((i.address + 4) & ~3) + disp
                    v = struct.unpack_from("<I", d, m.va2off(la))[0]
                    ann = "   ; %#x" % v
                    t = s_at(v)
                    if t: ann += ' "%s"' % t
                    else:
                        o2 = m.va2off(v)
                        if o2 is not None:
                            v2 = struct.unpack_from("<I", d, o2)[0]
                            t2 = s_at(v2)
                            if t2: ann += ' -> "%s"' % t2
                except Exception: pass
            print("%#010x  %-8s %s%s" % (i.address, i.mnemonic, i.op_str, ann))
            got = True; pos = i.address - start + i.size
        if not got: pos += 2
run(sys.argv[1], int(sys.argv[2],16), int(sys.argv[3],0))
