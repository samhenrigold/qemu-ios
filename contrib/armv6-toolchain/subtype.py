#!/usr/bin/env python3
"""Rewrite the cpusubtype in a thin 32-bit Mach-O header. subtype.py <file> <n>"""
import struct, sys
p, n = sys.argv[1], int(sys.argv[2])
b = bytearray(open(p, "rb").read())
magic = struct.unpack_from("<I", b, 0)[0]
assert magic == 0xFEEDFACE, "not a thin 32-bit little-endian Mach-O: %08x" % magic
cputype, old = struct.unpack_from("<ii", b, 4)
assert cputype == 12, "not CPU_TYPE_ARM: %d" % cputype
struct.pack_into("<i", b, 8, n)
open(p, "wb").write(b)
print("%s: cpusubtype %d -> %d" % (p, old, n))
