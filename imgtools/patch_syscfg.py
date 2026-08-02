"""Edit the SysCfg block in a NOR image (Mod#, Regn, SrNm, Batt, ...).

SAFE ALONGSIDE SHSH WRAPPING. SysCfg lives at 0x4000 and the first signed img3
starts at 0x8000, so writing here cannot disturb an image's signature. Verified
by offset, not assumed.

BUT: the NOR is load-bearing since the SHSH wrap landed. 3.x iBoot unwraps each
flash img3's SHSH under the device UID before verifying it, so a NOR whose
images carry plaintext SHSH fails validation SILENTLY - no error, just a black
screen where the boot logo should be and a device tree that will not load. If
you ever REGENERATE this file, go through imgtools/build_nor.py, which wraps by
default. Do not pass --no-wrap-shsh for a 3.x image. See memory
qemu-ios-shsh-wrap.

Note also the wrap is keyed on the emulated UID, so a wrapped NOR only validates
on a device with that UID - which is faithful, since a real restored NOR is
device-bound the same way.
"""
#!/usr/bin/env python3
"""Read and edit the SysCfg block in an iPod touch NOR image.

The NOR carries a small key/value block ("SCfg") that holds the per-unit
identity Apple programs at the factory: model number, region, serial number,
battery id.  iBoot copies those into the device tree it hands the kernel -
`model-number`, `region-info`, `serial-number`, ... - and that is where
Settings > General > About gets the Model row from.

    patch_syscfg.py --nor <file>                       # show the entries
    patch_syscfg.py --nor <file> --set Mod#=MB528 --set Regn=LL/A [--out new]

Values are written in place inside the existing 16-byte value slot, so the
block's size and layout never change.  Without --out the file is edited in
place.
"""

import argparse
import struct
import sys

MAGIC = b"SCfg"
HDR = 0x18          # magic, size, capacity, version, reserved, count
ENTRY = 0x14        # 4-byte tag + 16-byte value
VALUE = 0x10


def find_block(data):
    """Return the offset of the SysCfg block, or None."""
    # The tag is stored as a little-endian u32 of the four characters, i.e.
    # reversed on disk.
    needle = MAGIC[::-1]
    off = 0
    while True:
        off = data.find(needle, off)
        if off < 0:
            return None
        count = struct.unpack_from("<I", data, off + 0x14)[0]
        if 0 < count < 64:
            return off
        off += 4


def entries(data, base):
    count = struct.unpack_from("<I", data, base + 0x14)[0]
    for i in range(count):
        off = base + HDR + i * ENTRY
        tag = data[off:off + 4][::-1].decode("ascii", "replace")
        val = data[off + 4:off + 4 + VALUE]
        yield tag, val, off + 4


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--nor", required=True)
    ap.add_argument("--set", action="append", default=[], metavar="TAG=VALUE")
    ap.add_argument("--out")
    a = ap.parse_args()

    data = bytearray(open(a.nor, "rb").read())
    base = find_block(data)
    if base is None:
        raise SystemExit("no SysCfg block found in %s" % a.nor)
    ver, count = struct.unpack_from("<I", data, base + 0xc)[0], \
        struct.unpack_from("<I", data, base + 0x14)[0]
    print("SysCfg at 0x%x, version 0x%08x, %d entries" % (base, ver, count))

    wanted = dict(s.split("=", 1) for s in a.set)
    for tag, val, voff in entries(data, base):
        text = val.split(b"\0")[0].decode("ascii", "replace")
        if tag in wanted:
            new = wanted.pop(tag).encode("ascii")
            if len(new) > VALUE:
                raise SystemExit("%s: value longer than %d bytes" % (tag, VALUE))
            data[voff:voff + VALUE] = new.ljust(VALUE, b"\0")
            print("  %-6s %-18r -> %r" % (tag, text, new.decode()))
        else:
            print("  %-6s %r" % (tag, text))
    if wanted:
        raise SystemExit("tag(s) not present in this block: %s"
                         % ", ".join(wanted))

    if a.set:
        out = a.out or a.nor
        open(out, "wb").write(bytes(data))
        print("wrote %s" % out)


if __name__ == "__main__":
    sys.exit(main())
