#!/usr/bin/env python3
"""Decrypt an Apple 'encrcdsa' v2 disk image with a 36-byte vfdecrypt key."""
import hmac, hashlib, struct, sys
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

src, dst, keyhex = sys.argv[1], sys.argv[2], sys.argv[3]
key = bytes.fromhex(keyhex)
assert len(key) == 36, len(key)
aes_key, hmac_key = key[:16], key[16:]

f = open(src, "rb")
h = f.read(0x100)
assert h[:8] == b"encrcdsa"
blocksize, = struct.unpack(">I", h[52:56])
datasize, = struct.unpack(">Q", h[56:64])
dataoffset, = struct.unpack(">Q", h[64:72])

f.seek(dataoffset)
out = open(dst, "wb")
written = 0
n = 0
while written < datasize:
    ct = f.read(blocksize)
    if not ct:
        break
    iv = hmac.new(hmac_key, struct.pack(">I", n), hashlib.sha1).digest()[:16]
    dec = Cipher(algorithms.AES(aes_key), modes.CBC(iv)).decryptor()
    pt = dec.update(ct) + dec.finalize()
    take = min(len(pt), datasize - written)
    out.write(pt[:take])
    written += take
    n += 1
out.close()
print("wrote", written, "bytes,", n, "blocks")
