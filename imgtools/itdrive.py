#!/usr/bin/env python3
"""Drive an emulated iPod touch 2G over QMP: boot, tap, screenshot.

The LCD device registers a legacy absolute mouse handler
(`ipod_touch_lcd_mouse_event`, 0..32767 on both axes, Y inverted), so QMP
`input-send-event` reaches the touchscreen. The framebuffer's effective maximum
sample value is 1, so PPM dumps are normalised to 0..255 before being written
out as PNG - otherwise every screenshot looks solid black.
"""

import argparse
import json
import os
import socket
import subprocess
import sys
import time

QEMU = "/Users/shg/Developer/qemu-ios/build/qemu-system-arm"
FILES = "/Users/shg/Developer/qemu-ios-files"
W, H = 320, 480


class QMP:
    def __init__(self, host, port, timeout=180):
        deadline = time.time() + timeout
        while True:
            try:
                self.s = socket.create_connection((host, port), 5)
                break
            except OSError:
                if time.time() > deadline:
                    raise
                time.sleep(0.5)
        self.f = self.s.makefile("rwb")
        self._read()  # greeting
        self.cmd("qmp_capabilities")

    def _read(self):
        while True:
            line = self.f.readline()
            if not line:
                raise EOFError("qmp closed")
            msg = json.loads(line)
            if "event" in msg:
                continue
            return msg

    def cmd(self, name, **args):
        req = {"execute": name}
        if args:
            req["arguments"] = args
        self.f.write((json.dumps(req) + "\n").encode())
        self.f.flush()
        r = self._read()
        if "error" in r:
            raise RuntimeError("%s: %s" % (name, r["error"]))
        return r.get("return")

    def close(self):
        try:
            self.s.close()
        except OSError:
            pass


def abs_xy(x, y):
    """Screen pixels -> the absolute axis values the LCD handler expects.

    Y is *not* inverted here: `ipod_touch_lcd_mouse_event` already does the
    1 - y/2^15 flip itself, so we pass the same top-left-origin value a UI
    frontend would.
    """
    ax = int(x / W * 32768)
    ay = int(y / H * 32768)
    return max(0, min(32767, ax)), max(0, min(32767, ay))


def tap(q, x, y, hold=0.12):
    ax, ay = abs_xy(x, y)
    move = [
        {"type": "abs", "data": {"axis": "x", "value": ax}},
        {"type": "abs", "data": {"axis": "y", "value": ay}},
    ]
    q.cmd("input-send-event", events=move)
    q.cmd("input-send-event", events=move + [
        {"type": "btn", "data": {"button": "left", "down": True}}])
    time.sleep(hold)
    q.cmd("input-send-event", events=[
        {"type": "btn", "data": {"button": "left", "down": False}}])


def swipe(q, x0, y0, x1, y1, steps=12, dt=0.03):
    ax, ay = abs_xy(x0, y0)
    base = [{"type": "abs", "data": {"axis": "x", "value": ax}},
            {"type": "abs", "data": {"axis": "y", "value": ay}}]
    q.cmd("input-send-event", events=base)
    q.cmd("input-send-event", events=base + [
        {"type": "btn", "data": {"button": "left", "down": True}}])
    for i in range(1, steps + 1):
        x = x0 + (x1 - x0) * i / steps
        y = y0 + (y1 - y0) * i / steps
        ax, ay = abs_xy(x, y)
        q.cmd("input-send-event", events=[
            {"type": "abs", "data": {"axis": "x", "value": ax}},
            {"type": "abs", "data": {"axis": "y", "value": ay}}])
        time.sleep(dt)
    q.cmd("input-send-event", events=[
        {"type": "btn", "data": {"button": "left", "down": False}}])


def key(q, name, hold_ms=250):
    """Send a raw qcode key.

    NOT the hardware buttons any more. ipod_touch_kbd_button now puts those
    behind Command, so plain 'h' TYPES THE LETTER h and plain 'p' types p - a
    verification run once spent its whole sequence lost inside Safari's
    bookmarks because of this. For the buttons use send-key with a modifier:
    home = meta_l+shift+h, power = meta_l+l, volume = meta_l+minus / meta_l+equal.

    A brief press is easy for the guest to miss - the button is a GPIO level
    that iOS samples - so hold it for a quarter second by default.
    """
    q.cmd("send-key", keys=[{"type": "qcode", "data": name}], **{"hold-time": hold_ms})


def shot(q, path):
    """screendump + normalise. Returns (path, max_sample, nonzero_fraction)."""
    ppm = path + ".ppm"
    q.cmd("screendump", filename=ppm)
    for _ in range(40):
        if os.path.exists(ppm) and os.path.getsize(ppm) > 1000:
            break
        time.sleep(0.1)
    return normalize(ppm, path)


def normalize(ppm, png):
    with open(ppm, "rb") as f:
        data = f.read()
    # P6\n<w> <h>\n<maxval>\n<rgb...>
    parts, idx = [], 0
    while len(parts) < 4:
        while data[idx : idx + 1].isspace():
            idx += 1
        if data[idx : idx + 1] == b"#":
            while data[idx : idx + 1] != b"\n":
                idx += 1
            continue
        start = idx
        while not data[idx : idx + 1].isspace():
            idx += 1
        parts.append(data[start:idx])
    idx += 1
    w, h = int(parts[1]), int(parts[2])
    pix = bytearray(data[idx : idx + w * h * 3])
    hi = max(pix) if pix else 0
    if 0 < hi < 255:
        scale = 255.0 / hi
        pix = bytearray(min(255, int(v * scale)) for v in pix)
    out = b"P6\n%d %d\n255\n" % (w, h) + bytes(pix)
    tmp = png + ".norm.ppm"
    with open(tmp, "wb") as f:
        f.write(out)
    subprocess.run(["sips", "-s", "format", "png", tmp, "--out", png],
                   check=True, capture_output=True)
    os.remove(tmp)
    nz = sum(1 for v in pix if v) / max(1, len(pix))
    return png, hi, nz


def launch(nand, qmp_port, serial, extra=(), display="none", machine_opts=""):
    cmd = [
        QEMU, "-M",
        "iPod-Touch,bootrom=%s/bootrom_240_4,nand=%s,nor=%s/nor_n72ap.bin%s"
        % (FILES, nand, FILES, machine_opts),
        "-cpu", "max", "-m", "2G",
        "-serial", "file:" + serial,
        "-display", display,
        "-qmp", "tcp:127.0.0.1:%d,server=on,wait=off" % qmp_port,
    ]
    cmd += list(extra)
    log = open(serial + ".qemu", "wb")
    p = subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT)
    return p


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--nand", required=True)
    ap.add_argument("--qmp", type=int, default=4510)
    ap.add_argument("--out", required=True, help="output directory")
    ap.add_argument("--boot-wait", type=float, default=45)
    ap.add_argument("--machine-opts", default="",
                    help="extra -M options, e.g. ',usb-attached=on,...'")
    ap.add_argument("--nandrw", default=None,
                    help="writable copy-on-write overlay dir; lets guest writes "
                         "(crash logs, preferences) be read back on the host")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    serial = os.path.join(a.out, "serial.log")
    opts = a.machine_opts + (",nandrw=%s" % a.nandrw if a.nandrw else "")
    p = launch(a.nand, a.qmp, serial, machine_opts=opts)
    print("qemu pid", p.pid)
    q = QMP("127.0.0.1", a.qmp)
    print("qmp connected; waiting %.0fs for boot" % a.boot_wait)
    time.sleep(a.boot_wait)
    print(shot(q, os.path.join(a.out, "boot.png")))
    print("leaving qemu running; kill %d when done" % p.pid)


if __name__ == "__main__":
    main()
