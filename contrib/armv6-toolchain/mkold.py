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
leaving the classic tables it binds and slides from. What the compressed info
says is carried over into those tables, or the image is refused:
  binds     a non-lazy pointer the indirect symbol table names is bound by name
            already; any other bind (ObjC metadata's superclass, a CFString's
            isa) becomes a classic external relocation. No lazy or weak binds.
  rebases   (dylibs and bundles, which dyld slides) become classic local
            relocations, section-relative-free: r_address from the first
            segment, r_symbolnum the section, as 2.x dyld's doRebase reads them.
            A local non-lazy pointer (INDIRECT_SYMBOL_LOCAL) is left out: dyld
            slides those itself (doBindIndirectSymbolPointers), twice would be
            wrong. A rebase into a read-only segment or of a type other than
            a pointer is refused.
  executables  must be non-PIE (link with -no_pie): dyld never slides them.
An armv7 link against a 3.x+ target already routes calls through non-lazy
pointers, so link with -no_pie and this checks and converts the rest.
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
LC_CODE_SIGNATURE       = 0x1D
MH_PIE                  = 0x200000
MH_SPLIT_SEGS           = 0x20
MH_DYLIB, MH_BUNDLE     = 6, 8
S_NON_LAZY_SYMBOL_POINTERS = 0x6
INDIRECT_SYMBOL_LOCAL   = 0x80000000
# load commands whose data lives in __LINKEDIT (besides the symbol tables add_relocations moves)
LINKEDIT_USERS = {0x22, 0x2B, 0x2E, 0x80000033, 0x80000034}   # DYLD_INFO, DYLIB_CODE_SIGN_DRS, LINKER_OPTIMIZATION_HINT, EXPORTS_TRIE, CHAINED_FIXUPS

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


def legacy_convert(b, ncmds, flags, filetype):
    """What the classic tables need so LC_DYLD_INFO_ONLY can go (see --legacy): (why, fix), why
    None when sound. fix = (local relocations, external relocations, [(file offset, word)]),
    relocation_info records as 2.x dyld reads them."""
    segs, sects, info, symtab, dysym, off = [], [], None, None, None, 28
    for _ in range(ncmds):
        cmd, cmdsize = struct.unpack_from("<II", b, off)
        if cmd == LC_SEGMENT:
            vmaddr, vmsize, fileoff, _ = struct.unpack_from("<4I", b, off + 24)
            initprot = struct.unpack_from("<I", b, off + 44)[0]
            segs.append((vmaddr, vmsize, fileoff, initprot))
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
    none = (None, (b"", b"", []))
    if info is None:
        return none
    rebase_off, rebase_size, bind_off, bind_size, weak_off, weak_size, lazy_off, lazy_size, _, _ = info
    slid = filetype in (MH_DYLIB, MH_BUNDLE)
    if flags & MH_PIE:
        return "PIE (link with -no_pie)", None
    if flags & MH_SPLIT_SEGS:
        return "split segments (relocations would be from the first writable segment)", None
    if weak_size or lazy_size:
        return "has %s binds" % ("lazy" if lazy_size else "weak"), None
    if dysym[15] or dysym[17]:
        return "already has classic relocations", None
    symoff, nsyms, stroff, _ = symtab
    indoff, nind = dysym[12], dysym[13]
    iundef, nundef = dysym[4], dysym[5]

    def symname(index):
        strx = struct.unpack_from("<I", b, symoff + 12 * index)[0]
        return bytes(b[stroff + strx:b.index(0, stroff + strx)]).decode()

    def section_of(where):
        for n, s in enumerate(sects):
            if s[0] <= where < s[0] + s[1]:
                return n, s
        return None, None

    def fileoff(where):
        for vmaddr, vmsize, fo, _ in segs:
            if vmaddr <= where < vmaddr + vmsize:
                return fo + where - vmaddr
        return None

    def writable(where):
        return any(v <= where < v + vs and prot & 2 for v, vs, _, prot in segs)

    def reloc(where, symbolnum, extern):
        return struct.pack("<II", where - segs[0][0], symbolnum | (2 << 25) | (extern << 27))

    # binds: (address, name, addend)
    i, end, seg, addr, name, addend = bind_off, bind_off + bind_size, 0, 0, None, 0
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
            addend, i = uleb(b, i)                 # sleb, same byte framing
            if addend & (1 << 31):
                return "negative bind addend for %s" % name, None
        elif op == 0x70:
            seg, (addr, i) = imm, uleb(b, i)
        elif op == 0x80:
            step, i = uleb(b, i)
            addr = (addr + step) & 0xFFFFFFFF
        elif op == 0x90:
            binds.append((segs[seg][0] + addr, name, addend))
            addr += 4
        elif op == 0xA0:
            binds.append((segs[seg][0] + addr, name, addend))
            step, i = uleb(b, i)
            addr = (addr + step + 4) & 0xFFFFFFFF
        elif op == 0xB0:
            binds.append((segs[seg][0] + addr, name, addend))
            addr += imm * 4 + 4
        elif op == 0xC0:
            count, i = uleb(b, i)
            skip, i = uleb(b, i)
            for _ in range(count):
                binds.append((segs[seg][0] + addr, name, addend))
                addr += skip + 4
        else:
            return "unknown bind opcode %#x" % op, None
    undef = {symname(k): k for k in range(iundef, iundef + nundef)}
    extrel, patches, local_nl = [], [], set()
    for where, name, addend in binds:
        n, s = section_of(where)
        if s and s[2] == S_NON_LAZY_SYMBOL_POINTERS:
            slot = s[3] + (where - s[0]) // 4
            if slot >= nind or symname(struct.unpack_from("<I", b, indoff + 4 * slot)[0]) != name:
                return "bind of %s at %#x is not in the indirect symbol table" % (name, where), None
            if addend:
                return "bind of %s at %#x through a non-lazy pointer has an addend" % (name, where), None
            continue
        # classic dyld adds the symbol to the word in place: that word is the addend
        if name not in undef or not writable(where) or fileoff(where) is None:
            return "bind of %s at %#x has no classic form" % (name, where), None
        if addend:
            patches.append((fileoff(where), addend))
        extrel.append(reloc(where, undef[name], 1))
    for s in sects:
        if s[2] == S_NON_LAZY_SYMBOL_POINTERS:
            for k in range(s[1] // 4):
                if struct.unpack_from("<I", b, indoff + 4 * (s[3] + k))[0] == INDIRECT_SYMBOL_LOCAL:
                    local_nl.add(s[0] + 4 * k)

    # rebases: addresses
    i, end, seg, addr, kind = rebase_off, rebase_off + rebase_size, 0, 0, 1
    rebases = []
    while i < end:
        op, imm = b[i] & 0xF0, b[i] & 0x0F
        i += 1
        if op == 0x00:
            break
        elif op == 0x10:
            kind = imm
        elif op == 0x20:
            seg, (addr, i) = imm, uleb(b, i)
        elif op == 0x30:
            step, i = uleb(b, i)
            addr = (addr + step) & 0xFFFFFFFF
        elif op == 0x40:
            addr += imm * 4
        elif op in (0x50, 0x60, 0x70, 0x80):
            count, skip = imm, 0
            if op != 0x50:
                count, i = uleb(b, i)
            if op == 0x70:
                count, skip = 1, count
            elif op == 0x80:
                skip, i = uleb(b, i)
            for _ in range(count):
                if kind != 1:
                    return "rebase type %d at %#x (only pointers have a classic form)" % (
                        kind, segs[seg][0] + addr), None
                rebases.append(segs[seg][0] + addr)
                addr += 4 + skip
        else:
            return "unknown rebase opcode %#x" % op, None
    if not slid:
        rebases = []                               # an executable is never slid
    locrel = []
    for where in rebases:
        if where in local_nl:
            continue                               # dyld slides INDIRECT_SYMBOL_LOCAL pointers itself
        n, s = section_of(where)
        if s is None or not writable(where):
            return "rebase at %#x is not in a writable section (a text relocation)" % where, None
        locrel.append(reloc(where, n + 1, 0))
    return None, (b"".join(locrel), b"".join(extrel), patches)


def add_relocations(b, ncmds, locrel, extrel, patches):
    """Rebuild __LINKEDIT as the classic tables in ld's classic order (relocations, symbols, indirect
    symbols, strings: signing tools want the strings last) and point LC_SYMTAB/LC_DYSYMTAB at them. The
    compressed info, and a stale code signature (signing redoes it), are left out. Returns (bytes, note)."""
    for where, word in patches:
        struct.pack_into("<I", b, where, struct.unpack_from("<I", b, where)[0] + word)
    if not locrel and not extrel:
        return b, ""
    linkedit = symtab = dysym = None
    off = 28
    for _ in range(ncmds):
        cmd, cmdsize = struct.unpack_from("<II", b, off)
        if cmd == LC_SEGMENT and bytes(b[off + 8:off + 19]) == b"__LINKEDIT\0":
            linkedit = off
        elif cmd == LC_SYMTAB:
            symtab = off
        elif cmd == LC_DYSYMTAB:
            dysym = off
        elif cmd in LINKEDIT_USERS:
            sys.exit("load command %#x keeps data in __LINKEDIT; cannot rebuild it" % cmd)
        off += cmdsize
    vmaddr, vmsize, fileoff, filesize = struct.unpack_from("<4I", b, linkedit + 24)
    if fileoff + filesize != len(b):
        sys.exit("__LINKEDIT does not end the file; cannot add relocations")
    symoff, nsyms, stroff, strsize = struct.unpack_from("<4I", b, symtab + 8)
    d = list(struct.unpack_from("<18I", b, dysym + 8))
    if d[6] or d[8] or d[10]:
        sys.exit("a table of contents, module table or reference table; cannot rebuild __LINKEDIT")
    syms, ind, strs = b[symoff:symoff + 12 * nsyms], b[d[12]:d[12] + 4 * d[13]], b[stroff:stroff + strsize]
    le = bytearray(extrel + locrel)
    d[14], d[15], d[16], d[17] = (fileoff if extrel else 0), len(extrel) // 8, \
        (fileoff + len(extrel) if locrel else 0), len(locrel) // 8
    symoff = fileoff + len(le)
    le += syms
    d[12] = fileoff + len(le) if d[13] else 0
    le += ind
    stroff = fileoff + len(le)
    le += strs
    b = b[:fileoff] + le
    struct.pack_into("<4I", b, linkedit + 24, vmaddr, max(vmsize, (len(le) + 0xFFF) & ~0xFFF), fileoff, len(le))
    struct.pack_into("<4I", b, symtab + 8, symoff, nsyms, stroff, strsize)
    struct.pack_into("<18I", b, dysym + 8, *d)
    return b, " (%d rebases -> local relocations, %d binds -> external relocations)" % (
        len(locrel) // 8, len(extrel) // 8)


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
        why, fix = legacy_convert(b, ncmds, flags, filetype)
        if why:
            sys.exit("%s: cannot link for 2.x dyld: %s" % (path, why))
        b, relocated = add_relocations(b, ncmds, *fix)

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
            dropped.append("LC_DYLD_INFO_ONLY" + relocated)
        elif legacy and cmd == LC_CODE_SIGNATURE:
            dropped.append("LC_CODE_SIGNATURE (stale: sign after this)")
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


if __name__ == "__main__":
    main()
