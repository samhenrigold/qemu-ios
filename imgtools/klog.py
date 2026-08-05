#!/usr/bin/env python3
"""Read the guest's kernel log (printf/IOLog) out of a running emulator.

The iPod touch machine has no kernel serial console - `-serial` only ever
carries iBoot - so kernel-side messages have been invisible on this project.
They are not lost, though: XNU keeps them in its `msgbuf` ring in ordinary RAM,
and QMP's `pmemsave` can hand us all of RAM. This finds the ring and prints it.

That is how "AppleEmbeddedAudioDevice: could not start DMA: device is not ready"
and the AppleAMC self-test assertions were found - none of which appear anywhere
else.

    klog.py --qmp 4872                  # print the log
    klog.py --qmp 4872 --grep AMC       # only matching lines
    klog.py --ram dump.bin              # from an existing pmemsave dump

The ring is small (4 KiB) and wraps, so it holds only the most recent messages -
dump it soon after the thing you care about, not at the end of a long run.
"""

import argparse
import os
import re
import struct
import sys
import tempfile

sys.path.insert(0, os.path.dirname(__file__))
from itqmp import QMP, pmemsave as _pmemsave

RAM_BASE = 0x08000000
RAM_SIZE = 128 * 1024 * 1024
# The kernel is mapped at 0xc0000000 over physical 0x08000000.
VA_TO_PHYS = 0xB8000000
MSGBUF_MAGIC = 0x063061


def pmemsave(port, path):
    q = QMP("127.0.0.1", port, timeout=30)
    try:
        _pmemsave(q, path, RAM_BASE, RAM_SIZE)
    except RuntimeError as e:
        sys.exit("pmemsave failed: %s" % e)


def find_msgbuf(ram):
    """Locate struct msgbuf { magic, size, bufx, bufr, *bufc }."""
    needle = struct.pack("<I", MSGBUF_MAGIC)
    i = 0
    while True:
        i = ram.find(needle, i)
        if i < 0:
            return None
        size, bufx, bufr, bufc = struct.unpack("<4I", ram[i + 4:i + 20])
        if 0x1000 <= size <= 0x100000 and bufc > VA_TO_PHYS:
            return size, bufx, bufc - VA_TO_PHYS - RAM_BASE
        i += 4


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qmp", type=int, help="QMP port of a running emulator")
    ap.add_argument("--ram", help="an existing pmemsave dump instead")
    ap.add_argument("--grep", help="only lines matching this regex")
    a = ap.parse_args()

    tmp = None
    if a.ram:
        path = a.ram
    elif a.qmp:
        tmp = tempfile.mktemp(suffix=".ram")
        pmemsave(a.qmp, tmp)
        path = tmp
    else:
        sys.exit("need --qmp or --ram")

    with open(path, "rb") as f:
        ram = f.read()
    if tmp:
        os.unlink(tmp)

    hit = find_msgbuf(ram)
    if not hit:
        sys.exit("no msgbuf found - is the guest past kernel init?")
    size, bufx, off = hit

    # bufx is the write cursor into a ring, so the oldest text follows it.
    buf = ram[off:off + size]
    text = (buf[bufx:] + buf[:bufx]).decode("ascii", "replace")
    text = re.sub(r"[^\x20-\x7e\n]", "", text)

    pat = re.compile(a.grep) if a.grep else None
    for line in text.split("\n"):
        if line.strip() and (pat is None or pat.search(line)):
            print(line)


if __name__ == "__main__":
    main()
