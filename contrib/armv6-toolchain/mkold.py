#!/usr/bin/env python3
"""Turn a modern thin armv7 Mach-O into something 2010 dyld will load.

Strips the load commands that did not exist in 2010 (LC_VERSION_MIN_IPHONEOS,
LC_SOURCE_VERSION, LC_ENCRYPTION_INFO, LC_DATA_IN_CODE, LC_FUNCTION_STARTS,
LC_UUID, LC_BUILD_VERSION) and rewrites cpusubtype to armv6.

Removing commands only shrinks the header, so every file offset in the file
stays valid; we just zero-fill the tail of the load-command region so the
padding between the header and __text is untouched.

    mkold.py <macho> [--subtype N] [--legacy]

--legacy also drops LC_DYLD_INFO_ONLY, which 2.x dyld refuses (0x80000022),
leaving the classic tables it binds from. That is only sound when the
compressed info says nothing the classic tables do not: every bind a
non-lazy pointer the indirect symbol table names, no lazy or weak binds, and
a non-PIE executable (rebases never apply). An armv7 link against a 3.x+
target already routes calls through non-lazy pointers (__picsymbolstub5), so
link with -no_pie and this checks the rest, refusing anything else.
"""
import struct, sys

LC_SEGMENT              = 0x01
LC_UUID                 = 0x1B
LC_VERSION_MIN_IPHONEOS = 0x25
LC_FUNCTION_STARTS      = 0x26
LC_DATA_IN_CODE         = 0x29
LC_SOURCE_VERSION       = 0x2A
LC_ENCRYPTION_INFO      = 0x21
LC_BUILD_VERSION        = 0x32
LC_MAIN                 = 0x80000028
LC_SYMTAB               = 0x02
LC_DYSYMTAB             = 0x0B
LC_DYLD_INFO_ONLY       = 0x80000022
MH_PIE                  = 0x200000
S_NON_LAZY_SYMBOL_POINTERS = 0x6

DROP = {LC_UUID, LC_VERSION_MIN_IPHONEOS, LC_FUNCTION_STARTS, LC_DATA_IN_CODE,
        LC_SOURCE_VERSION, LC_ENCRYPTION_INFO, LC_BUILD_VERSION}

NAMES = {LC_UUID: "LC_UUID", LC_VERSION_MIN_IPHONEOS: "LC_VERSION_MIN_IPHONEOS",
         LC_FUNCTION_STARTS: "LC_FUNCTION_STARTS", LC_DATA_IN_CODE: "LC_DATA_IN_CODE",
         LC_SOURCE_VERSION: "LC_SOURCE_VERSION", LC_ENCRYPTION_INFO: "LC_ENCRYPTION_INFO",
         LC_BUILD_VERSION: "LC_BUILD_VERSION"}


def uleb(b, i):
    v = shift = 0
    while True:
        c = b[i]
        i += 1
        v |= (c & 0x7F) << shift
        shift += 7
        if c < 0x80:
            return v, i


def legacy_problem(b, ncmds, flags):
    """None if LC_DYLD_INFO_ONLY adds nothing the classic tables lack (see --legacy), else why."""
    segs, sects, info, symtab, dysym, off = [], [], None, None, None, 28
    for _ in range(ncmds):
        cmd, cmdsize = struct.unpack_from("<II", b, off)
        if cmd == LC_SEGMENT:
            segs.append(struct.unpack_from("<I", b, off + 24)[0])
            for k in range(struct.unpack_from("<I", b, off + 48)[0]):
                s = off + 56 + 68 * k
                addr, size = struct.unpack_from("<II", b, s + 32)
                sflags, reserved1 = struct.unpack_from("<II", b, s + 56)
                sects.append((addr, size, sflags & 0xFF, reserved1))
        elif cmd == LC_DYLD_INFO_ONLY:
            info = struct.unpack_from("<10I", b, off + 8)
        elif cmd == LC_SYMTAB:
            symtab = struct.unpack_from("<4I", b, off + 8)
        elif cmd == LC_DYSYMTAB:
            dysym = struct.unpack_from("<18I", b, off + 8)
        off += cmdsize
    if info is None:
        return None
    _, _, bind_off, bind_size, weak_off, weak_size, lazy_off, lazy_size, _, _ = info
    if flags & MH_PIE:
        return "PIE (link with -no_pie)"
    if weak_size or lazy_size:
        return "has %s binds" % ("lazy" if lazy_size else "weak")
    symoff, _, stroff, _ = symtab
    indoff, nind = dysym[12], dysym[13]

    def symname(index):
        strx = struct.unpack_from("<I", b, symoff + 12 * index)[0]
        return bytes(b[stroff + strx:b.index(0, stroff + strx)]).decode()

    i, end, seg, addr, name, done = bind_off, bind_off + bind_size, 0, 0, None, 0
    binds = []
    while i < end:
        op, imm = b[i] & 0xF0, b[i] & 0x0F
        i += 1
        if op == 0x00:
            break
        elif op in (0x10, 0x30, 0x50):
            pass
        elif op == 0x20:
            _, i = uleb(b, i)
        elif op == 0x40:
            j = b.index(0, i)
            name, i = bytes(b[i:j]).decode(), j + 1
        elif op == 0x60:
            _, i = uleb(b, i)                  # addend: sleb, same byte framing
        elif op == 0x70:
            seg, (addr, i) = imm, uleb(b, i)
        elif op == 0x80:
            step, i = uleb(b, i)
            addr = (addr + step) & 0xFFFFFFFF
        elif op == 0x90:
            binds.append((segs[seg] + addr, name))
            addr += 4
        elif op == 0xA0:
            binds.append((segs[seg] + addr, name))
            step, i = uleb(b, i)
            addr = (addr + step + 4) & 0xFFFFFFFF
        elif op == 0xB0:
            binds.append((segs[seg] + addr, name))
            addr += imm * 4 + 4
        elif op == 0xC0:
            count, i = uleb(b, i)
            skip, i = uleb(b, i)
            for _ in range(count):
                binds.append((segs[seg] + addr, name))
                addr += skip + 4
        else:
            return "unknown bind opcode %#x" % op
    for where, name in binds:
        hit = [s for s in sects if s[0] <= where < s[0] + s[1]]
        if not hit or hit[0][2] != S_NON_LAZY_SYMBOL_POINTERS:
            return "bind of %s at %#x is not a non-lazy pointer" % (name, where)
        slot = hit[0][3] + (where - hit[0][0]) // 4
        if slot >= nind or symname(struct.unpack_from("<I", b, indoff + 4 * slot)[0]) != name:
            return "bind of %s at %#x is not in the indirect symbol table" % (name, where)
    return None


def main():
    path = sys.argv[1]
    subtype = 6
    if "--subtype" in sys.argv:
        subtype = int(sys.argv[sys.argv.index("--subtype") + 1])

    b = bytearray(open(path, "rb").read())
    magic = struct.unpack_from("<I", b, 0)[0]
    if magic != 0xFEEDFACE:
        sys.exit("not a thin 32-bit LE Mach-O: %08x (run lipo -thin first)" % magic)
    cputype, oldsub, filetype, ncmds, sizeofcmds, flags = struct.unpack_from("<iiIIII", b, 4)

    # __TEXT vmaddr, needed to turn LC_MAIN's entryoff into an absolute pc.
    text_vmaddr, off = 0, 28
    for _ in range(ncmds):
        cmd, cmdsize = struct.unpack_from("<II", b, off)
        if cmd == LC_SEGMENT and bytes(b[off + 8:off + 14]) == b"__TEXT":
            text_vmaddr = struct.unpack_from("<I", b, off + 24)[0]
        off += cmdsize

    legacy = "--legacy" in sys.argv
    if legacy:
        why = legacy_problem(b, ncmds, flags)
        if why:
            sys.exit("%s: cannot link for 2.x dyld: %s" % (path, why))

    kept, dropped, off = [], [], 28
    for _ in range(ncmds):
        cmd, cmdsize = struct.unpack_from("<II", b, off)
        chunk = bytes(b[off:off + cmdsize])
        if cmd == LC_MAIN:
            # 2010 dyld predates LC_MAIN (iOS 6) and refuses the image outright.
            # Rebuild it as the LC_UNIXTHREAD it would have been in 2010, with
            # pc pointing straight at _main. There is no crt1 in the way, so
            # _main must never return -- see tester.c, which calls _exit().
            entryoff = struct.unpack_from("<Q", b, off + 8)[0]
            regs = [0] * 17
            regs[15] = text_vmaddr + entryoff          # pc
            chunk = struct.pack("<IIII", 0x5, 84, 1, 17) + struct.pack("<17I", *regs)
            kept.append(chunk)
            dropped.append("LC_MAIN->LC_UNIXTHREAD(pc=0x%x)" % regs[15])
        elif legacy and cmd == LC_DYLD_INFO_ONLY:
            dropped.append("LC_DYLD_INFO_ONLY")
        elif (cmd & ~0x80000000) in DROP and cmd < 0x80000000:
            dropped.append(NAMES.get(cmd, hex(cmd)))
        else:
            kept.append(chunk)
        off += cmdsize

    new = b"".join(kept)
    # Zero the whole old load-command region, then lay the kept commands back down.
    b[28:28 + sizeofcmds] = b"\x00" * sizeofcmds
    b[28:28 + len(new)] = new
    struct.pack_into("<iiIIII", b, 4, cputype, subtype, filetype, len(kept), len(new), flags)

    open(path, "wb").write(b)
    print("%s: cpusubtype %d -> %d; ncmds %d -> %d; dropped %s"
          % (path, oldsub, subtype, ncmds, len(kept), ", ".join(dropped) or "(none)"))


main()
