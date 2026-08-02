#!/usr/bin/env python3
"""Repack an iPod touch 2G (n72ap) NOR image from an IPSW's all_flash directory.

The 1 MiB SPI NOR served by hw/arm/ipod_touch_nor_spi.c is a flat image with
four regions:

    0x000000  IMG2 superblock (see below)
    0x004000  SysCfg ('gfCS') -- serial numbers, BMac/WMac, battery data
    0x008000  the image area: img3 containers packed back to back
    0x0fc000  nvram atoms, including the "common" atom that holds boot-args

Only the image area is rebuilt; everything else is copied from a base image so
the MAC addresses and serials the guest expects survive.

IMG2 superblock (little endian), as read off LLB's parser (7E18 LLB @0x619e):

    0x00  u32  magic          'IMG2', stored as the bytes "2GMI"
    0x04  u32  granularity    0x40
    0x08  u32  start_hi       )  image area = granularity * (start_hi + start_lo)
    0x0c  u32  start_lo       )  = 0x40 * (0 + 0x200) = 0x8000
    0x10  u32  span           bytes available for images = granularity * span
    0x30  u32  checksum       crc32 of bytes [0x00, 0x30)

LLB validates the magic and the crc32, then walks the image area reading a
0x14-byte img3 header at a time.  For each entry it requires the 'Img3' magic
and that fullSize be a multiple of granularity, then advances by fullSize.  A
non-'Img3' magic (i.e. the zero fill after the last image) ends the walk.

One thing more is needed for 3.x, and without it nothing in the image area is
loadable.  A 3.x iBoot does not verify the SHSH of a flash-resident image as it
finds it: image3_validate() first unwraps it, decrypting the 128-byte signature
in place with AES-CBC under a key the AES engine derives from the device UID
(iBoot 7E18 @0x0ff110a6 -- AES-decrypt SHSH_KDF_CONST with the UID key, then
decrypt the SHSH with the result).  A real restore writes the images with their
SHSH already wrapped for that specific device, so the unwrap recovers the
genuine Apple signature and the RSA check passes.

The IPSW's all_flash copies carry a plaintext SHSH, so pasting them in
unmodified means iBoot decrypts a signature that was never encrypted, gets
garbage, and rejects the image -- silently, in the case of the boot logo.  So
pack them the way a restore would: wrap each SHSH under the UID key of the
device this NOR is for.  That is what makes iBoot load and draw the Apple logo
by itself, and load the device tree, with no patching of iBoot at all.

2.x iBoot has no such step (every SHSH in a stock 2.1.1 NOR dump is plaintext
and verifies raw), so use --no-wrap-shsh when building a 2.x image.

Shipped IPSW img3 files are not granularity-aligned, so packing an image means:
pad it with zeros up to the next granularity boundary and rewrite fullSize (and
only fullSize) to the padded size.  sizeNoPack and sigCheckArea are left alone,
which keeps the signature blob's coverage intact.  This rule reproduces 9 of the
10 images in the stock 5F138 NOR byte for byte; the tenth, iBoot, differs only
in its 128-byte RSA signature because the stock dump came off a device that was
restored with a personalized iBoot.

Usage:
    build_nor.py --base nor_n72ap.bin \\
                 --all-flash .../all_flash.n72ap.production \\
                 --out nor_n72ap_7E18.bin
"""

import argparse
import glob
import os
import struct
import sys
import zlib

IMG2_MAGIC = b"2GMI"
IMG3_MAGIC = b"3gmI"
IMG3_HDR = 0x14
NVRAM_OFF = 0x0FC000

# The image types the stock 5F138 NOR carries, in the order it carries them.
# batterycharging0/1 and batteryfull ship in all_flash but are not in NOR.
DEFAULT_ORDER = [
    "illb", "ibot", "dtre", "logo", "nsrv",
    "bat0", "bat1", "recm", "glyC", "glyP",
]

# The 16 bytes iBoot runs through the UID key to get the SHSH wrapping key.
SHSH_KDF_CONST = bytes.fromhex("db1f5b33606c5f1c1934aa66589c0661")

# The UID the emulated S5L8720 reports; see key_uid in ipod_touch_aes.h.  A NOR
# built with this wrapping only validates on a device with this UID, exactly as
# a real restored NOR only validates on the device it was restored to.
DEFAULT_UID_KEY = bytes.fromhex("0123456789ABCDEF0123456789ABCDEF")


def _aes_cbc(key, data, encrypt):
    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
    ctx = Cipher(algorithms.AES(key), modes.CBC(b"\x00" * 16))
    op = ctx.encryptor() if encrypt else ctx.decryptor()
    return op.update(data) + op.finalize()


def shsh_wrap_key(uid_key):
    """The key iBoot derives from the device UID to unwrap a flash SHSH."""
    return _aes_cbc(uid_key, SHSH_KDF_CONST, encrypt=False)


def wrap_shsh(data, uid_key):
    """Encrypt an img3's SHSH tag so iBoot's in-place unwrap recovers it.

    The SHSH tag sits past sigCheckArea, so it is not covered by the digest the
    signature is over -- rewriting it does not invalidate anything.
    """
    blob = bytearray(data)
    key = shsh_wrap_key(uid_key)
    nopack = struct.unpack("<I", blob[8:12])[0]
    off, end = IMG3_HDR, IMG3_HDR + nopack
    while off + 12 <= end:
        tag, total, dlen = struct.unpack("<4sII", blob[off:off + 12])
        if total == 0:
            break
        if tag[::-1] == b"SHSH" and dlen % 16 == 0:
            body = bytes(blob[off + 12:off + 12 + dlen])
            blob[off + 12:off + 12 + dlen] = _aes_cbc(key, body, encrypt=True)
            return bytes(blob), True
        off += total
    return bytes(blob), False


def parse_img3(data):
    """Return (ident, full_size) for an img3 blob."""
    if data[:4] != IMG3_MAGIC:
        raise ValueError("not an img3 (magic %r)" % data[:4])
    full = struct.unpack("<I", data[4:8])[0]
    ident = data[16:20][::-1].decode("latin1")
    return ident, full


def scan_all_flash(path):
    """Map img3 ident -> (filename, bytes) for every img3 in an all_flash dir."""
    out = {}
    for f in sorted(glob.glob(os.path.join(path, "*.img3"))):
        data = open(f, "rb").read()
        ident, full = parse_img3(data)
        if full != len(data):
            print("warning: %s declares fullSize %d but is %d bytes"
                  % (os.path.basename(f), full, len(data)), file=sys.stderr)
        if ident in out:
            raise ValueError("duplicate img3 type %r in %s" % (ident, path))
        out[ident] = (os.path.basename(f), data)
    return out


def pack_img3(data, granularity):
    """Zero-pad an img3 to the granularity and rewrite its fullSize."""
    padded = (len(data) + granularity - 1) & ~(granularity - 1)
    blob = bytearray(data) + b"\x00" * (padded - len(data))
    struct.pack_into("<I", blob, 4, padded)
    return bytes(blob)


def read_img2(nor):
    if nor[:4] != IMG2_MAGIC:
        raise ValueError("base image has no IMG2 superblock")
    gran, start_hi, start_lo, span = struct.unpack("<IIII", nor[4:20])
    stored = struct.unpack("<I", nor[0x30:0x34])[0]
    actual = zlib.crc32(nor[:0x30]) & 0xFFFFFFFF
    if stored != actual:
        print("warning: base IMG2 checksum is 0x%08x, computed 0x%08x"
              % (stored, actual), file=sys.stderr)
    return gran, start_hi, start_lo, span


def build(base_path, all_flash, out_path, order, verbose=True,
          uid_key=DEFAULT_UID_KEY):
    nor = bytearray(open(base_path, "rb").read())
    gran, start_hi, start_lo, _ = read_img2(nor)
    image_start = gran * (start_hi + start_lo)

    available = scan_all_flash(all_flash)
    missing = [t for t in order if t not in available]
    if missing:
        raise SystemExit("all_flash has no img3 of type(s): %s" % ", ".join(missing))

    # Wipe the old image area, keeping SysCfg below it and nvram above it.
    nor[image_start:NVRAM_OFF] = b"\x00" * (NVRAM_OFF - image_start)

    off = image_start
    for ident in order:
        name, data = available[ident]
        wrapped = False
        if uid_key is not None:
            data, wrapped = wrap_shsh(data, uid_key)
        blob = pack_img3(data, gran)
        end = off + len(blob)
        if end > NVRAM_OFF:
            raise SystemExit(
                "image area overflows the nvram partition at 0x%06x "
                "(%s would end at 0x%06x)" % (NVRAM_OFF, ident, end))
        nor[off:end] = blob
        if verbose:
            print("  0x%06x  %-5s %-34s %7d -> %7d%s"
                  % (off, ident, name, len(data), len(blob),
                     "  shsh wrapped" if wrapped else ""))
        off = end

    # The stock header's span is the absolute end offset in granules, i.e. it
    # over-counts by image_start.  Keep that convention: it is a bound on the
    # walk, and being generous only means LLB trusts the zero fill to stop it.
    span = (off + gran - 1) // gran
    struct.pack_into("<I", nor, 0x10, span)
    struct.pack_into("<I", nor, 0x30, zlib.crc32(bytes(nor[:0x30])) & 0xFFFFFFFF)

    open(out_path, "wb").write(bytes(nor))
    if verbose:
        print("images end at 0x%06x, span=0x%x granules, crc32=0x%08x"
              % (off, span, struct.unpack("<I", nor[0x30:0x34])[0]))
        print("wrote %s (%d bytes)" % (out_path, len(nor)))


def verify(path):
    """Walk a NOR image the way LLB does and print what it would find."""
    nor = open(path, "rb").read()
    gran, start_hi, start_lo, span = read_img2(nor)
    off = gran * (start_hi + start_lo)
    remaining = gran * span
    print("granularity 0x%x, image area 0x%06x, span %d bytes" % (gran, off, remaining))
    n = 0
    while off + IMG3_HDR <= len(nor):
        if nor[off:off + 4] != IMG3_MAGIC:
            print("  0x%06x  end of walk (magic %r)" % (off, nor[off:off + 4]))
            break
        ident, full = parse_img3(nor[off:off + IMG3_HDR])
        if full > remaining:
            print("  0x%06x  %s rejected: fullSize %d > remaining %d"
                  % (off, ident, full, remaining))
            break
        if full % gran:
            print("  0x%06x  %s rejected: fullSize %d not a multiple of 0x%x"
                  % (off, ident, full, gran, ))
            break
        print("  0x%06x  %-5s %7d" % (off, ident, full))
        off += full
        remaining -= full
        n += 1
    print("%d images" % n)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--base", help="NOR image to take IMG2/SysCfg/nvram from")
    ap.add_argument("--all-flash", help="IPSW all_flash.n72ap.production directory")
    ap.add_argument("--out", help="output NOR image")
    ap.add_argument("--types", default=",".join(DEFAULT_ORDER),
                    help="comma-separated img3 types to pack, in order")
    ap.add_argument("--verify", metavar="NOR",
                    help="instead of building, walk a NOR image as LLB would")
    ap.add_argument("--uid-key", default=DEFAULT_UID_KEY.hex(),
                    help="device UID key (32 hex digits) to wrap each SHSH for")
    ap.add_argument("--no-wrap-shsh", action="store_true",
                    help="leave the SHSH tags in plaintext; 2.x iBoot wants this")
    args = ap.parse_args()

    if args.verify:
        verify(args.verify)
        return
    if not (args.base and args.all_flash and args.out):
        ap.error("--base, --all-flash and --out are all required")
    uid = None if args.no_wrap_shsh else bytes.fromhex(args.uid_key)
    build(args.base, args.all_flash, args.out, args.types.split(","), uid_key=uid)


if __name__ == "__main__":
    main()
