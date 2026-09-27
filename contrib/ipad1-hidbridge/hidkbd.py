#!/usr/bin/env python3
"""Host side of hidbridge: send boot-keyboard reports to the guest over usbmuxd.

    hidkbd.py [--tcp HOST:PORT] type TEXT      type ASCII text (\\n = Return, \\b = Backspace, \\t = Tab)
    hidkbd.py [--tcp HOST:PORT] keys CODE...   press+release macOS virtual key codes (kVK_*, decimal or 0x..)
    hidkbd.py --selfcheck

Default transport is usbmuxd (/var/run/usbmuxd, first device); --tcp talks straight to a forwarded port
(e.g. iproxy 5213 5213 for a real iPad). Wire format: docs/ipad1/keyboard-and-network.md §1.
"""
import os
import plistlib
import socket
import struct
import sys
import time

PORT = 5213
SHIFT = 0x02

# macOS kVK_* virtual key code -> HID keyboard usage (page 7). Modifiers map to 0xe0-0xe7.
MAC_TO_HID = {
    0x00: 0x04, 0x0B: 0x05, 0x08: 0x06, 0x02: 0x07, 0x0E: 0x08, 0x03: 0x09, 0x05: 0x0A, 0x04: 0x0B,
    0x22: 0x0C, 0x26: 0x0D, 0x28: 0x0E, 0x25: 0x0F, 0x2E: 0x10, 0x2D: 0x11, 0x1F: 0x12, 0x23: 0x13,
    0x0C: 0x14, 0x0F: 0x15, 0x01: 0x16, 0x11: 0x17, 0x20: 0x18, 0x09: 0x19, 0x0D: 0x1A, 0x07: 0x1B,
    0x10: 0x1C, 0x06: 0x1D,
    0x12: 0x1E, 0x13: 0x1F, 0x14: 0x20, 0x15: 0x21, 0x17: 0x22, 0x16: 0x23, 0x1A: 0x24, 0x1C: 0x25,
    0x19: 0x26, 0x1D: 0x27,
    0x24: 0x28, 0x35: 0x29, 0x33: 0x2A, 0x30: 0x2B, 0x31: 0x2C, 0x1B: 0x2D, 0x18: 0x2E, 0x21: 0x2F,
    0x1E: 0x30, 0x2A: 0x31, 0x29: 0x33, 0x27: 0x34, 0x32: 0x35, 0x2B: 0x36, 0x2F: 0x37, 0x2C: 0x38,
    0x39: 0x39, 0x0A: 0x64,
    0x7A: 0x3A, 0x78: 0x3B, 0x63: 0x3C, 0x76: 0x3D, 0x60: 0x3E, 0x61: 0x3F, 0x62: 0x40, 0x64: 0x41,
    0x65: 0x42, 0x6D: 0x43, 0x67: 0x44, 0x6F: 0x45,
    0x73: 0x4A, 0x74: 0x4B, 0x75: 0x4C, 0x77: 0x4D, 0x79: 0x4E,
    0x7C: 0x4F, 0x7B: 0x50, 0x7D: 0x51, 0x7E: 0x52,
    0x3B: 0xE0, 0x38: 0xE1, 0x3A: 0xE2, 0x37: 0xE3, 0x3E: 0xE4, 0x3C: 0xE5, 0x3D: 0xE6, 0x36: 0xE7,
}

_US = "abcdefghijklmnopqrstuvwxyz1234567890\n\x1b\b\t -=[]\\\0;'`,./"      # index + 4 = usage
_US_SHIFT = "ABCDEFGHIJKLMNOPQRSTUVWXYZ!@#$%^&*()\0\0\0\0\0_+{}|\0:\"~<>?"


def ascii_to_hid(ch):
    """(modifiers, usage) for one US-layout character."""
    if ch in _US and ch != "\0":
        return 0, 0x04 + _US.index(ch)
    if ch in _US_SHIFT and ch != "\0":
        return SHIFT, 0x04 + _US_SHIFT.index(ch)
    raise ValueError("no key for %r" % ch)


def report(mods, keys=()):
    return struct.pack("8B", mods, 0, *(list(keys) + [0] * 6)[:6])


def taps(pairs):
    """press+release reports for a sequence of (modifiers, usage)."""
    return b"".join(report(m, [u]) + report(0) for m, u in pairs)


def mac_keycode_taps(codes):
    out = []
    for c in codes:
        u = MAC_TO_HID[c]
        out.append((1 << (u - 0xE0), 0) if u >= 0xE0 else (0, u))
    return taps(out)


def usbmux_connect(port):
    s = socket.socket(socket.AF_UNIX)
    s.connect(os.environ.get("USBMUXD_SOCKET_ADDRESS", "/var/run/usbmuxd"))
    tag = 0

    def ask(msg):
        nonlocal tag
        tag += 1
        body = plistlib.dumps(dict(msg, ClientVersionString="hidkbd", ProgName="hidkbd"))
        s.sendall(struct.pack("<IIII", 16 + len(body), 1, 8, tag) + body)
        n = struct.unpack("<I", s.recv(4, socket.MSG_WAITALL))[0]
        return plistlib.loads(s.recv(n - 4, socket.MSG_WAITALL)[12:])

    devs = ask({"MessageType": "ListDevices"})["DeviceList"]
    if not devs:
        raise SystemExit("usbmuxd: no device")
    r = ask({"MessageType": "Connect", "DeviceID": devs[0]["DeviceID"], "PortNumber": socket.htons(port)})
    if r.get("Number"):
        raise SystemExit("usbmuxd: connect to %d refused (%s)" % (port, r.get("Number")))
    return s


def selfcheck():
    assert ascii_to_hid("a") == (0, 0x04) and ascii_to_hid("Z") == (SHIFT, 0x1D)
    assert ascii_to_hid("1") == (0, 0x1E) and ascii_to_hid("0") == (0, 0x27) and ascii_to_hid("!") == (SHIFT, 0x1E)
    assert ascii_to_hid("\n") == (0, 0x28) and ascii_to_hid("\b") == (0, 0x2A) and ascii_to_hid(" ") == (0, 0x2C)
    assert ascii_to_hid("\\") == (0, 0x31) and ascii_to_hid(";") == (0, 0x33) and ascii_to_hid("/") == (0, 0x38)
    assert ascii_to_hid("?") == (SHIFT, 0x38) and ascii_to_hid('"') == (SHIFT, 0x34) and ascii_to_hid("~") == (SHIFT, 0x35)
    assert report(SHIFT, [0x04]) == bytes([2, 0, 4, 0, 0, 0, 0, 0])
    assert mac_keycode_taps([0x00, 0x38]) == report(0, [4]) + report(0) + report(SHIFT) + report(0)
    assert len(set(MAC_TO_HID.values())) == len(MAC_TO_HID)


def main(argv):
    selfcheck()
    tcp = None
    if argv[:1] == ["--tcp"]:
        tcp, argv = argv[1], argv[2:]
    if len(argv) < 2 or argv[0] not in ("type", "keys"):
        raise SystemExit(__doc__ if argv != ["--selfcheck"] else 0)
    if argv[0] == "type":
        text = " ".join(argv[1:]).encode().decode("unicode_escape")
        data = taps(ascii_to_hid(c) for c in text)
    else:
        data = mac_keycode_taps(int(c, 0) for c in argv[1:])
    if tcp:
        host, port = tcp.rsplit(":", 1)
        s = socket.create_connection((host, int(port)))
    else:
        s = usbmux_connect(PORT)
    for i in range(0, len(data), 8):     # one report per 8 bytes; pace them like a typist
        s.sendall(data[i:i + 8])
        time.sleep(0.02)
    s.close()


if __name__ == "__main__":
    main(sys.argv[1:])
