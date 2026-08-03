#!/usr/bin/env python3
"""Turn a modern thin armv7 Mach-O into something 2010 dyld will load.

Strips the load commands that did not exist in 2010 (LC_VERSION_MIN_IPHONEOS,
LC_SOURCE_VERSION, LC_ENCRYPTION_INFO, LC_DATA_IN_CODE, LC_FUNCTION_STARTS,
LC_UUID, LC_BUILD_VERSION) and rewrites cpusubtype to armv6.

Removing commands only shrinks the header, so every file offset in the file
stays valid; we just zero-fill the tail of the load-command region so the
padding between the header and __text is untouched.

    mkold.py <macho> [--subtype N]
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

DROP = {LC_UUID, LC_VERSION_MIN_IPHONEOS, LC_FUNCTION_STARTS, LC_DATA_IN_CODE,
        LC_SOURCE_VERSION, LC_ENCRYPTION_INFO, LC_BUILD_VERSION}

NAMES = {LC_UUID: "LC_UUID", LC_VERSION_MIN_IPHONEOS: "LC_VERSION_MIN_IPHONEOS",
         LC_FUNCTION_STARTS: "LC_FUNCTION_STARTS", LC_DATA_IN_CODE: "LC_DATA_IN_CODE",
         LC_SOURCE_VERSION: "LC_SOURCE_VERSION", LC_ENCRYPTION_INFO: "LC_ENCRYPTION_INFO",
         LC_BUILD_VERSION: "LC_BUILD_VERSION"}


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
