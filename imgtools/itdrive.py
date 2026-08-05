#!/usr/bin/env python3
"""Drive an emulated iPod touch 2G over QMP: boot, tap, screenshot.

The LCD device registers a legacy absolute mouse handler
(`ipod_touch_lcd_mouse_event`, 0..32767 on both axes, Y inverted), so QMP
`input-send-event` reaches the touchscreen. The framebuffer's effective maximum
sample value is 1, so PPM dumps are normalised to 0..255 before being written
out as PNG - otherwise every screenshot looks solid black.
"""

import argparse
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(__file__))
from itqmp import QMP, abs_xy, tap, swipe, finger, pinch, key, shot, normalize  # noqa: F401

QEMU = "/Users/shg/Developer/qemu-ios/build/qemu-system-arm"
FILES = "/Users/shg/Developer/qemu-ios-files"
W, H = 320, 480


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
