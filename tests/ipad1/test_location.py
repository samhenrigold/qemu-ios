#!/usr/bin/env python3
"""itwebproxy's Wi-Fi location answer (it_location_response in contrib/it-webproxy/itwebproxy.c).

Feeds it the request 3.2 locationd sent on a real boot (one cell tower stub,
the fake AP's BSSID) and checks the answer places that BSSID at the position
in CONFIG.location, in every proxy mode. Needs a built itwebproxy.
"""
import pathlib, struct, subprocess, tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
PROXY = ROOT / "contrib/it-webproxy/itwebproxy"
# Captured from 7B500 locationd (AppleLocationServer -> the proxy address).
BODY = bytes.fromhex("00010005656e5f55530000000b332e322e322e3742353030000000010000001f"
                     "0a080800100018002000120f0a0d323a303a35653a31303a303a3118002000")


def varint(b, i):
    v = s = 0
    while True:
        v |= (b[i] & 0x7f) << s
        s += 7
        i += 1
        if not b[i - 1] & 0x80:
            return v, i


def fields(b):
    i = 0
    while i < len(b):
        key, i = varint(b, i)
        if key & 7 == 2:
            n, i = varint(b, i)
            yield key >> 3, b[i:i + n]
            i += n
        else:
            v, i = varint(b, i)
            yield key >> 3, v


def ask(conf):
    request = (b"POST /clls/wloc HTTP/1.1\r\nHost: 10.0.2.100:3128\r\n"
               b"Content-Length: %d\r\n\r\n" % len(BODY)) + BODY
    out = subprocess.run([str(PROXY), conf], input=request, capture_output=True, timeout=10).stdout
    head, body = out.split(b"\r\n\r\n", 1)
    assert head.startswith(b"HTTP/1.0 200"), head
    assert body[:2] == b"\0\1", body[:10]
    rtype, length = struct.unpack(">II", body[2:10])
    assert rtype == 1 and length == len(body) - 10, (rtype, length)
    aps = [dict(fields(v)) for n, v in fields(body[10:]) if n == 2]
    assert len(aps) == 1 and aps[0][1] == b"2:0:5e:10:0:1", aps
    where = dict(fields(aps[0][2]))
    signed = lambda v: v - (1 << 64) if v >= 1 << 63 else v
    return signed(where[1]) / 1e8, signed(where[2]) / 1e8, where[3]


with tempfile.TemporaryDirectory() as d:
    conf = f"{d}/proxy.conf"
    for mode in ("direct\n", "off\n", "archive\n20090909\n"):
        open(conf, "w").write(mode)
        lat, lon, acc = ask(conf)
        assert (round(lat, 5), round(lon, 5), acc) == (37.3349, -122.00898, 30), (mode, lat, lon, acc)
        open(conf + ".location", "w").write("51.50073 -0.12463 20\n")
        lat, lon, acc = ask(conf)
        assert (round(lat, 5), round(lon, 5), acc) == (51.50073, -0.12463, 20), (mode, lat, lon, acc)
        pathlib.Path(conf + ".location").unlink()
print("location: PASS (default, host-set, and every proxy mode)")
