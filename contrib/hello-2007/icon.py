#!/usr/bin/env python3
"""icon.py OUT.png Hello|Tilt -- the apps' 57x57 home-screen icons (SpringBoard adds the gloss and corners)."""
import struct, sys, zlib

GLYPHS = {"H": ["10001", "10001", "10001", "11111", "10001", "10001", "10001"],
          "i": ["00100", "00000", "01100", "00100", "00100", "00100", "01110"]}
S = 57


def png(rows):
    raw = b"".join(b"\0" + bytes(c for px in r for c in px) for r in rows)
    chunk = lambda t, d: struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", S, S, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def hello():
    rows = [[(int(30 + 40 * y / S), int(60 + 80 * y / S), int(140 + 90 * y / S)) for x in range(S)] for y in range(S)]
    scale, x0, y0 = 4, 6, 14
    for gi, ch in enumerate("Hi"):
        for gy, line in enumerate(GLYPHS[ch]):
            for gx, bit in enumerate(line):
                if bit == "1":
                    for dy in range(scale):
                        for dx in range(scale):
                            rows[y0 + gy * scale + dy][x0 + gi * 24 + gx * scale + dx] = (255, 255, 255)
    return rows


def tilt():
    """Tilt's level: rings and a crosshair on slate, the ball off-center."""
    rows = [[(18, 23, 31)] * S for _ in range(S)]
    c = S // 2
    for y in range(S):
        for x in range(S):
            d = ((x - c) ** 2 + (y - c) ** 2) ** 0.5
            if any(abs(d - r) < 0.9 for r in (8, 16, 24)) or (x == c or y == c) and d < 26:
                rows[y][x] = (77, 97, 122)
            if ((x - c - 9) ** 2 + (y - c + 7) ** 2) ** 0.5 < 8:
                rows[y][x] = (242, 140, 38)
    return rows


open(sys.argv[1], "wb").write(png({"Hello": hello, "Tilt": tilt}[sys.argv[2]]()))
