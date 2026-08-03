#!/usr/bin/env python3
"""Decode `SBW 0x...` lines from idevicesyslog back into SpringBoard event names.

Companion to `sb_workflow_log.py --syslog`, which logs each workflow event as
the address of its constant CFString rather than its text (syslog is a C printf
and cannot render %@).  SpringBoard is not PIE on iOS 3, so those addresses are
the link-time __cfstring VAs and map straight back to the message.

    imgtools/sb_workflow_decode.py <syslog capture> <SpringBoard binary>
"""
import re, struct, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from macho import Macho


def cfstrings(path):
    """{va: text} for every constant CFString in the image."""
    m = Macho(path)
    d = m.data
    seg = [s for s in m.segs if s[0] == "__DATA"][0]
    sec = [x for x in seg[5] if x[0] == "__cfstring"][0]
    va, size = sec[2], sec[3]
    out = {}
    for a in range(va, va + size, 16):
        try:
            _, _, p, ln = struct.unpack_from("<4I", d, m.va2off(a))
            o = m.va2off(p)
            s = d[o:o + ln]
            if 0 < ln < 300 and all(32 <= c < 127 for c in s):
                out[a] = s.decode()
        except Exception:
            pass
    return out


def main():
    table = cfstrings(sys.argv[2])
    for line in open(sys.argv[1], errors="replace"):
        g = re.search(r"(\w{3} \w{3}\s+\d+ [\d:]+).*SBW (0x[0-9a-f]+)", line)
        if g:
            print("%s  %-10s %s"
                  % (g.group(1), g.group(2),
                     table.get(int(g.group(2), 16), "<unknown>")))


if __name__ == "__main__":
    main()
