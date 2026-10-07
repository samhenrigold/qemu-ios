#!/usr/bin/env python3
"""mkold.py --legacy on a synthetic armv7 dylib: its rebases and data binds come out as the classic
relocations 2.x dyld slides and binds from (checked by applying them as its doRebase,
doBindIndirectSymbolPointers and external-relocation pass do), and an image whose rebases have no
classic form is refused rather than passed with them dropped."""
import os, struct, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
MKOLD = os.path.join(ROOT, "contrib/armv6-toolchain/mkold.py")
LOCAL = 0x80000000


def seg(name, vmaddr, size, fileoff, prot, sects):
    cmd = struct.pack("<II16s8I", 1, 56 + 68 * len(sects), name, vmaddr, size, fileoff, size, prot, prot,
                      len(sects), 0)
    for sname, addr, ssize, flags, r1 in sects:
        cmd += struct.pack("<16s16s9I", sname, name, addr, ssize, fileoff + addr - vmaddr, 2, 0, 0, flags, r1, 0)
    return cmd


def dylib(rebase_ops, text_rebase=False):
    """__TEXT 0x0 (a __text at 0x800), __DATA 0x1000: __nl_symbol_ptr (a local slot, _ext_func),
    __data (two pointers into the image, _OBJC_CLASS_$_Foo, _ext_data + 8), __LINKEDIT 0x2000."""
    names = [b"_OBJC_CLASS_$_Foo", b"_ext_data", b"_ext_func"]       # undefined, sorted
    strtab = b"\0" + b"".join(n + b"\0" for n in names)
    stroffs = [1 + sum(len(n) + 1 for n in names[:k]) for k in range(3)]
    symtab = b"".join(struct.pack("<IBBhI", stroffs[k], 1, 0, 0x100, 0) for k in range(3))   # N_EXT|N_UNDF
    binds = (b"\x11" + b"\x40_ext_func\0" + b"\x51" + b"\x71\x04" + b"\x90" +
             b"\x40_OBJC_CLASS_$_Foo\0" + b"\x71\x10" + b"\x90" +
             b"\x40_ext_data\0" + b"\x60\x08" + b"\x90" + b"\x00")
    indirect = struct.pack("<II", LOCAL, 2)
    le = 0x2000
    rebase_off = le
    bind_off = rebase_off + len(rebase_ops)
    sym_off = (bind_off + len(binds) + 3) & ~3
    ind_off = sym_off + len(symtab)
    str_off = ind_off + len(indirect)
    end = str_off + len(strtab)
    cmds = seg(b"__TEXT", 0, 0x1000, 0, 5, [(b"__text", 0x800, 0x10, 0x80000400, 0)])
    cmds += seg(b"__DATA", 0x1000, 0x1000, 0x1000, 3, [(b"__nl_symbol_ptr", 0x1000, 8, 6, 0),
                                                        (b"__data", 0x1008, 0x10, 0, 0)])
    cmds += struct.pack("<II16s8I", 1, 56, b"__LINKEDIT", le, 0x1000, le, end - le, 1, 1, 0, 0)
    cmds += struct.pack("<12I", 0x80000022, 48, rebase_off, len(rebase_ops), bind_off, len(binds), 0, 0, 0, 0, 0, 0)
    cmds += struct.pack("<6I", 2, 24, sym_off, 3, str_off, len(strtab))
    cmds += struct.pack("<20I", 0xB, 80, 0, 0, 0, 0, 0, 3, 0, 0, 0, 0, 0, 0, ind_off, 2, 0, 0, 0, 0)
    ncmds = 6
    b = bytearray(end)
    struct.pack_into("<7I", b, 0, 0xFEEDFACE, 12, 9, 6, ncmds, len(cmds), 0x100085)
    b[28:28 + len(cmds)] = cmds
    struct.pack_into("<II", b, 0x1000, 0x1008, 0)                     # local NL slot -> __data; _ext_func
    struct.pack_into("<4I", b, 0x1008, 0x800, 0x1010, 0, 0)           # -> __text, -> __data; two binds
    if text_rebase:
        struct.pack_into("<I", b, 0x804, 0x1008)
    b[rebase_off:rebase_off + len(rebase_ops)] = rebase_ops
    b[bind_off:bind_off + len(binds)] = binds
    b[sym_off:sym_off + len(symtab)] = symtab
    b[ind_off:ind_off + len(indirect)] = indirect
    b[str_off:str_off + len(strtab)] = strtab
    return bytes(b)


# 0x1000 (the local NL slot), then 0x1008 and 0x100c
REBASES = b"\x11" + b"\x21\x00" + b"\x51" + b"\x41" + b"\x52" + b"\x00"


def mkold(data):
    with tempfile.TemporaryDirectory() as t:
        p = os.path.join(t, "lib.dylib")
        open(p, "wb").write(data)
        r = subprocess.run([sys.executable, MKOLD, p, "--legacy"], capture_output=True, text=True)
        return r, open(p, "rb").read()


def load(b, slide, symbols):
    """Map the file as 2.x dyld would at `slide` and apply what it applies; returns word(vmaddr)."""
    mem = bytearray(b[:0x2000])
    ncmds = struct.unpack_from("<I", b, 16)[0]
    off, dysym, cmds = 28, None, set()
    for _ in range(ncmds):
        cmd, size = struct.unpack_from("<II", b, off)
        cmds.add(cmd)
        if cmd == 0xB:
            dysym = struct.unpack_from("<18I", b, off + 8)
        off += size
    assert 0x80000022 not in cmds, "LC_DYLD_INFO_ONLY survived"
    extreloff, nextrel, locreloff, nlocrel = dysym[14:18]
    word = lambda a: struct.unpack_from("<I", mem, a)[0]
    put = lambda a, v: struct.pack_into("<I", mem, a, v & 0xFFFFFFFF)
    for k in range(nlocrel):                                          # doRebase
        addr, info = struct.unpack_from("<II", b, locreloff + 8 * k)
        assert info & 0xFFFFFF and (info >> 25) & 3 == 2 and info >> 28 == 0 and not info >> 27 & 1
        put(addr, word(addr) + slide)
    put(0x1000, word(0x1000) + slide)                                 # INDIRECT_SYMBOL_LOCAL
    put(0x1004, symbols["_ext_func"])                                 # the indirect table's bind
    names = ["_OBJC_CLASS_$_Foo", "_ext_data", "_ext_func"]
    for k in range(nextrel):                                          # external relocations
        addr, info = struct.unpack_from("<II", b, extreloff + 8 * k)
        assert info >> 27 & 1 and (info >> 25) & 3 == 2
        put(addr, word(addr) + symbols[names[info & 0xFFFFFF]])
    return word, nlocrel, nextrel


def main():
    r, out = mkold(dylib(REBASES))
    assert r.returncode == 0, r.stderr
    assert "2 rebases -> local relocations, 2 binds -> external relocations" in r.stdout, r.stdout
    slide = 0x40000
    symbols = {"_ext_func": 0x31000001, "_OBJC_CLASS_$_Foo": 0x38000000, "_ext_data": 0x39000000}
    word, nloc, next_ = load(out, slide, symbols)
    assert (nloc, next_) == (2, 2)
    want = {0x1000: 0x1008 + slide, 0x1004: 0x31000001, 0x1008: 0x800 + slide, 0x100c: 0x1010 + slide,
            0x1010: 0x38000000, 0x1014: 0x39000008}
    for a, v in want.items():
        assert word(a) == v, "%#x: %#x, want %#x" % (a, word(a), v)
    # a rebase in __TEXT (a text relocation) and one of another type have no classic form: refused
    text = b"\x11" + b"\x21\x00" + b"\x51" + b"\x41" + b"\x52" + b"\x20\x84\x10" + b"\x51" + b"\x00"
    r, _ = mkold(dylib(text, text_rebase=True))
    assert r.returncode and "text relocation" in r.stderr, (r.returncode, r.stderr)
    r, _ = mkold(dylib(b"\x12" + b"\x21\x08" + b"\x51" + b"\x00"))
    assert r.returncode and "rebase type 2" in r.stderr, (r.returncode, r.stderr)
    print("PASS: rebases -> local relocations (local NL slot left to dyld), data binds -> external "
          "relocations with the addend in place; text and non-pointer rebases refused")


if __name__ == "__main__":
    main()
