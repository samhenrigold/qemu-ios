#!/usr/bin/env python3
"""Expected answers for contrib/it-cctest; compare against a serial log.

    tests/ipad1/cctest.py SERIAL.LOG      # prints mismatches, exit 1 if any
"""
import hashlib, hmac, re, sys
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

def xorshift(seed, n):
    out = bytearray(n); s = seed & 0xffffffff
    for i in range(n):
        s ^= (s << 13) & 0xffffffff; s ^= s >> 17; s ^= (s << 5) & 0xffffffff
        out[i] = s & 0xff
    return bytes(out)

def fnv(b):
    h = 2166136261
    for x in b: h = ((h ^ x) * 16777619) & 0xffffffff
    return h

KEY, IV = xorshift(7, 20), xorshift(9, 16)

def expected(op, n, al):
    """The set of right answers: an AES line only names n & ~15, which can
    come from more than one test length (16 and 17, say)."""
    if op.startswith("aes"):
        out = set()
        for base in [l for l in LENS if l & ~15 == n]:
            d = xorshift(1000 + base + al, n)
            c = Cipher(algorithms.AES(KEY[:16]), modes.CBC(IV)).encryptor().update(d)
            out.add(fnv(c) if op == "aesenc" else fnv(d))
        return out
    d = xorshift(1000 + n + al, n)
    if op == "memcpy": return {fnv(d)}
    if op == "sha1": return {fnv(hashlib.sha1(d).digest())}
    return {fnv(hmac.new(KEY, d, hashlib.sha1).digest())}

LENS = [1, 13, 15, 16, 17, 63, 64, 65, 100, 511, 1023, 1024, 1025, 1440, 1460, 1500, 2048,
        4095, 4096, 4097, 8191, 16384, 16385, 32768, 65536]

bad = total = 0
for line in open(sys.argv[1], errors="replace"):
    m = re.match(r"(sha1|hmac|hmacp|memcpy|aesenc|aesdec|aesdecs) (\d+) (\d) ([0-9a-f]{8})", line)
    if not m: continue
    op, n, al, got = m.group(1), int(m.group(2)), int(m.group(3)), int(m.group(4), 16)
    total += 1
    if got not in expected(op, n, al):
        bad += 1
        print("MISMATCH", line.strip())
print("%d cases, %d mismatches" % (total, bad))
sys.exit(1 if bad or not total else 0)
