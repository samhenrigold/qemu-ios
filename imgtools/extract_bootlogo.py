#!/usr/bin/env python3
"""Extract and decrypt the boot Apple logo from a NOR image, for IT_INJECT_LOGO.

iBoot cannot load the logo itself: its image_load rejects the personalised
signature before it ever reaches the GID decrypt (measured over the gdbstub -
it returns -1), which is why the screen is black for the whole of iBoot's life.
IT_INJECT_LOGO hands it the decrypted image instead, exactly as IT_INJECT_DT
does for the device tree.

    extract_bootlogo.py --nor nor_7E18.bin --out applelogo_7E18.bin

The output is the raw iBootIm container iBoot expects on the success path:
magic "iBootIm\\0", a 'lzss'/'argb'-or-'grey' header, then the compressed image.
iBoot decompresses and blits it, so nothing here has to understand lzss.

The payload key is the published per-build one, the same value the emulated AES
engine already keeps in it_gid_blobs[] - decrypting here rather than in the
device model keeps the blob inspectable and the model free of image parsing.
"""

import argparse
import struct
import sys

try:
    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
except ImportError:
    sys.exit("needs `cryptography` (pip install cryptography)")

# build -> (IV, key) for the applelogo img3 payload. Same table as
# hw/arm/ipod_touch_aes.c's it_gid_blobs[], which is keyed on the KBAG
# ciphertext rather than on the order the images are loaded in.
KEYS = {
    "7E18": ("f246cfefd363c2ec38927473be6f6d45",
             "afa37377de73fdf1e2ad6b0117cd2847"),
}


def img3_tags(buf, off):
    """Yield (tag, data_offset, data_len) for every tag in the img3 at `off`."""
    magic, full, size, _sig, typ = struct.unpack_from("<4sIII4s", buf, off)
    if magic != b"3gmI":
        raise ValueError("no img3 at 0x%x" % off)
    p = off + 20
    while p < off + 20 + size:
        tag, totlen, datalen = struct.unpack_from("<4sII", buf, p)
        if totlen == 0:
            break
        yield tag[::-1].decode(), p + 12, datalen, totlen
        p += totlen


def find_img3(buf, want):
    off = 0
    while True:
        off = buf.find(b"3gmI", off)
        if off < 0:
            return None
        try:
            _m, _f, _s, _sig, typ = struct.unpack_from("<4sIII4s", buf, off)
        except struct.error:
            return None
        if typ[::-1].decode(errors="replace") == want:
            return off
        off += 4


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--nor", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--build", default="7E18")
    ap.add_argument("--type", default="logo",
                    help="img3 type: logo, recm, nsrv, bat0 ...")
    a = ap.parse_args()

    nor = open(a.nor, "rb").read()
    off = find_img3(nor, a.type)
    if off is None:
        sys.exit("no '%s' img3 in %s" % (a.type, a.nor))

    data = None
    for tag, doff, dlen, tlen in img3_tags(nor, off):
        if tag == "DATA":
            # Decrypt the WHOLE padded region, then truncate to datalen.
            #
            # This is the one subtlety in the file. The DATA tag declares
            # datalen 0x1c3a (7226) but occupies 0x1c4c bytes, i.e. 0x1c40
            # (7232) of payload - a whole number of AES blocks, because the
            # container pads the ciphertext. Decrypting only the first 7226
            # bytes leaves the last ten as an undecryptable partial block, and
            # the lzss stream then runs out 20 bytes early: iBoot decompresses
            # 30700 where it wants width*height*bpp = 96*160*2 = 30720, fails
            # its `decompressed == expected` check at 0x0ff133ba, and silently
            # draws nothing. Decrypt 7232, keep 7226, and it comes out exact.
            enc_len = tlen - 12
            data = nor[doff:doff + enc_len]
            keep = dlen
    if data is None:
        sys.exit("'%s' img3 has no DATA tag" % a.type)
    if len(data) % 16:
        sys.exit("DATA payload is 0x%x bytes, not a whole number of AES blocks"
                 % len(data))

    iv, key = (bytes.fromhex(x) for x in KEYS[a.build])
    dec = Cipher(algorithms.AES(key), modes.CBC(iv)).decryptor()
    plain = (dec.update(data) + dec.finalize())[:keep]

    if plain[:8] != b"iBootIm\0":
        sys.exit("decrypt produced %r, not an iBootIm container - wrong key or "
                 "wrong build?" % plain[:8])

    comp = plain[0x0c:0x10][::-1].decode(errors="replace")
    fmt = plain[0x10:0x14][::-1].decode(errors="replace")
    w, h = struct.unpack_from("<HH", plain, 0x14)
    if comp != "lzss" or fmt not in ("argb", "grey"):
        print("warning: iBoot only accepts 'lzss' with 'argb' or 'grey'; "
              "this is %r/%r" % (comp, fmt), file=sys.stderr)

    with open(a.out, "wb") as f:
        f.write(plain)
    print("%s: img3 '%s' at 0x%x, %d bytes -> %s (%s/%s, %dx%d)"
          % (a.nor, a.type, off, len(plain), a.out, comp, fmt, w, h))


if __name__ == "__main__":
    main()
