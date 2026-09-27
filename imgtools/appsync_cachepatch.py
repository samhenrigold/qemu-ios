#!/usr/bin/env python3
"""Patch MISValidateSignature to return success inside a dyld shared cache, located by SYMBOL
(never a fixed offset). This is the amfid-global half of AppSync: with libmis's signature check
forced to succeed, amfid approves every image, so the ldid-signed AppSync dylib (and decrypted
apps) load. The installd info-dict and SpringBoard gates stay in contrib/appsync's dylib.

    appsync_cachepatch.py CACHE          # report the symbol, VA, file offset, prologue
    appsync_cachepatch.py CACHE --patch  # patch in place (prologue byte-checked first)

Symbol lookup parses each image's own LC_SYMTAB in the cache linkedit; no SDK, no offset table.
Thumb `MISValidateSignature` prologue -> `movs r0,#0 ; bx lr` (00 20 70 47), the iPod's patch 3.
"""
import struct, sys

TARGET = "_MISValidateSignature"
# Thumb `movs r0,#0` (0x2000) + `bx lr` (0x4770), little-endian bytes.
PATCH = bytes.fromhex("00207047")
# Accept only a real Thumb function entry: a `push {...,lr}` (16-bit 0xB5xx, or 32-bit push.w
# 0xE92D with the LR bit) so we never scribble on the wrong bytes.
def looks_like_thumb_entry(b):
    hw = struct.unpack_from("<H", b, 0)[0]
    if (hw & 0xFF00) == 0xB500:                 # push {..., lr}
        return True
    if hw == 0xE92D:                            # push.w {...}
        return (struct.unpack_from("<H", b, 2)[0] & 0x4000) != 0   # LR in the reg list
    return False


def find_symbol(data, name):
    """(vaddr, is_thumb) of name, by scanning each cached image's LC_SYMTAB. Raises if absent."""
    mo, mc, io, ic = struct.unpack_from("<IIII", data, 0x10)
    maps = [struct.unpack_from("<QQQ", data, mo + 32 * i) for i in range(mc)]

    def a2o(a):
        for addr, size, off in maps:
            if addr <= a < addr + size:
                return off + a - addr
        raise ValueError("VA %#x not in any mapping" % a)

    want = name.encode()
    for i in range(ic):
        base = struct.unpack_from("<Q", data, io + 32 * i)[0]
        h = a2o(base)
        if struct.unpack_from("<I", data, h)[0] != 0xFEEDFACE:
            continue
        ncmds = struct.unpack_from("<I", data, h + 16)[0]
        off = h + 28
        for _ in range(ncmds):
            cmd, sz = struct.unpack_from("<II", data, off)
            if cmd == 0x02:                     # LC_SYMTAB
                symoff, nsyms, stroff, strsize = struct.unpack_from("<IIII", data, off + 8)
                # symoff/stroff are cache file offsets (unslid linkedit) for these old caches.
                for s in range(nsyms):
                    e = symoff + s * 12
                    n_strx, n_type, n_sect, n_desc, n_value = struct.unpack_from("<IBBHI", data, e)
                    if not n_value or (n_type & 0x0e) != 0x0e:   # N_SECT only
                        continue
                    nm = data[stroff + n_strx:data.index(b"\0", stroff + n_strx)]
                    if nm == want:
                        return n_value, bool(n_desc & 0x0008)     # N_ARM_THUMB_DEF
            off += sz
    raise KeyError("%s not found in cache image symbol tables" % name)


def patch_cache(path, do_patch=True):
    """Locate and (optionally) patch MISValidateSignature. Returns a one-line status string.
    Raises on a prologue that is not a Thumb function entry (fails loudly, never guesses)."""
    data = bytearray(open(path, "rb").read())
    mo, mc = struct.unpack_from("<II", data, 0x10)
    maps = [struct.unpack_from("<QQQ", data, mo + 32 * i) for i in range(mc)]
    a2o = lambda a: next(off + a - addr for addr, size, off in maps if addr <= a < addr + size)

    va, thumb = find_symbol(data, TARGET)
    foff = a2o(va)
    cur = bytes(data[foff:foff + 4])
    if cur == PATCH:
        return "%s already patched (VA %#x off %#x)" % (TARGET, va, foff)
    if not thumb or not looks_like_thumb_entry(cur):
        raise ValueError("%s prologue %s at %#x is not a Thumb function entry — refusing to patch"
                         % (TARGET, cur.hex(), foff))
    if not do_patch:
        return "would patch %s %s -> %s at %#x" % (TARGET, cur.hex(), PATCH.hex(), foff)
    data[foff:foff + 4] = PATCH
    open(path, "wb").write(data)
    return "patched %s %s -> %s (VA %#x off %#x)" % (TARGET, cur.hex(), PATCH.hex(), va, foff)


def main():
    path = sys.argv[1]
    print(patch_cache(path, do_patch="--patch" in sys.argv[2:]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
