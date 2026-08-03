#!/usr/bin/env python3
"""Build a SpringBoard with a trampoline around -[SBDisplay updateStatusBar:].

Separates the two candidates for the ~2.17 s stall inside the preactivate-push
handler at 0x11b80: the status-bar update, or animateApplicationActivation:.
Markers: 0x11 = about to call updateStatusBar:, 0x22 = it returned.
"""
import sys, shutil, struct
sys.path.insert(0, '/Users/shg/Developer/qemu-ios/imgtools')
from macho import Macho
import capstone

SYSLOG, MSGSEND = 0xb1d00, 0xb1bc8
THUNK, FMT = 0x11018, 0x11048
CALLSITE = 0x11bd0          # blx objc_msgSend for [display updateStatusBar:0.4]

def enc(addr, target, blx):
    """32-bit Thumb BL/BLX immediate."""
    pc = addr + 4
    base = pc & ~3 if blx else pc
    off = target - base
    assert off % (4 if blx else 2) == 0, hex(off)
    v = off >> 1
    S = (v >> 23) & 1
    i1, i2 = (v >> 22) & 1, (v >> 21) & 1
    J1, J2 = (~(i1 ^ S)) & 1, (~(i2 ^ S)) & 1
    hi = 0xF000 | (S << 10) | ((v >> 11) & 0x3FF)
    lo = (0xC000 if blx else 0xD000) | (J1 << 13) | (J2 << 11) | (v & 0x7FF)
    if blx:
        lo &= ~1
    return struct.pack('<2H', hi, lo)

def adr(rd, addr, target):
    off = target - ((addr + 4) & ~3)
    assert 0 <= off <= 1020 and off % 4 == 0, hex(off)
    return struct.pack('<H', 0xA000 | (rd << 8) | (off >> 2))

thunk  = struct.pack('<H', 0xB5F0)                       # push {r4-r7,lr}
thunk += b''.join(struct.pack('<H', 0x1C00 | (i << 3) | (i + 4)) for i in range(4))
thunk += struct.pack('<H', 0x2004)                       # movs r0,#4
thunk += adr(1, THUNK + len(thunk), FMT)
thunk += struct.pack('<H', 0x2211)                       # movs r2,#0x11
thunk += enc(THUNK + len(thunk), SYSLOG, True)
thunk += b''.join(struct.pack('<H', 0x1C00 | ((i + 4) << 3) | i) for i in range(4))
thunk += enc(THUNK + len(thunk), MSGSEND, True)
thunk += struct.pack('<H', 0x2004)                       # movs r0,#4
thunk += adr(1, THUNK + len(thunk), FMT)
thunk += struct.pack('<H', 0x2222)                       # movs r2,#0x22
thunk += enc(THUNK + len(thunk), SYSLOG, True)
thunk += struct.pack('<H', 0xBDF0)                       # pop {r4-r7,pc}
assert THUNK + len(thunk) <= FMT, (hex(THUNK + len(thunk)), hex(FMT))

src, dst = sys.argv[1], sys.argv[2]
shutil.copyfile(src, dst)
m = Macho(dst)
with open(dst, 'r+b') as f:
    f.seek(m.va2off(THUNK)); f.write(thunk)
    f.seek(m.va2off(FMT));   f.write(b'SBW %p\0')
    f.seek(m.va2off(CALLSITE)); f.write(enc(CALLSITE, THUNK, False))

md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_THUMB)
d = Macho(dst).data
print('--- trampoline ---')
o = Macho(dst).va2off(THUNK)
for i in md.disasm(d[o:o + len(thunk)], THUNK):
    print('%08x  %-6s %s' % (i.address, i.mnemonic, i.op_str))
print('--- patched call site ---')
o = Macho(dst).va2off(CALLSITE)
for i in md.disasm(d[o:o + 4], CALLSITE):
    print('%08x  %-6s %s' % (i.address, i.mnemonic, i.op_str))
