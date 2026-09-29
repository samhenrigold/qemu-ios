#!/usr/bin/env python3
"""Patch decrypted 7B500 iBEC: bgcolor -> md, go -> mw, code in go's old body."""
import struct, sys
BASE, GO_BODY, GO_END = 0x5ff00000, 0x5ff00b54, 0x5ff00c10
ENTRIES = {0x5ff16da4: ("md", 0), 0x5ff16dd4: ("mw", None)}  # table entry -> (name, code offset)

def elf_text(path):
    e = open(path, "rb").read()
    shoff, = struct.unpack_from("<I", e, 0x20); shnum, shstrndx = struct.unpack_from("<HH", e, 0x30)
    sh = [struct.unpack_from("<10I", e, shoff + i * 40) for i in range(shnum)]
    names = sh[shstrndx][4]
    for s in sh:
        if e[names + s[0]:].startswith(b".text\0"):
            return e[s[4]:s[4] + s[5]]

code = elf_text(sys.argv[3] if len(sys.argv) > 3 else "mdmw.o")
mw_off = code.index(bytes.fromhex("10b5"))  # push {r4, lr} opens mw
assert len(code) <= GO_END - GO_BODY, len(code)
d = bytearray(open(sys.argv[1], "rb").read())
d[GO_BODY - BASE:GO_BODY - BASE + len(code)] = code
for entry, (name, off) in ENTRIES.items():
    nameptr, = struct.unpack_from("<I", d, entry - BASE)
    old = d[nameptr - BASE:d.index(b"\0", nameptr - BASE)]
    assert len(name) <= len(old), old
    d[nameptr - BASE:nameptr - BASE + len(name) + 1] = name.encode() + b"\0"
    struct.pack_into("<I", d, entry - BASE + 4, (GO_BODY + (mw_off if off is None else off)) | 1)
    print(f"{old.decode()} -> {name} @ {GO_BODY + (mw_off if off is None else off):#x}")
open(sys.argv[2], "wb").write(d)
print(f"{len(code)} bytes of code at {GO_BODY:#x}")
