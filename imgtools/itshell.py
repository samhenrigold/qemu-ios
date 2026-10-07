#!/usr/bin/env python3
"""Send a short action script to an already-running emulator over QMP.

    itshell.py --qmp 4510 --out DIR tap:160,325 wait:1 shot:home key:h shot:after

Actions: tap:X,Y  swipe:X0,Y0,X1,Y1  key:NAME  wait:SECONDS  shot:LABEL
"""

import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import itdrive as it


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qmp", type=int, default=4510)
    ap.add_argument("--out", required=True)
    ap.add_argument("actions", nargs="+")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    q = it.QMP("127.0.0.1", a.qmp, timeout=15)
    for act in a.actions:
        kind, _, arg = act.partition(":")
        if kind == "tap":
            x, y = (float(v) for v in arg.split(","))
            it.tap(q, x, y)
        elif kind == "swipe":
            x0, y0, x1, y1 = (float(v) for v in arg.split(","))
            it.swipe(q, x0, y0, x1, y1)
        elif kind == "key":
            it.key(q, arg)
        elif kind == "wait":
            time.sleep(float(arg))
        elif kind == "shot":
            print(act, it.shot(q, os.path.join(a.out, arg + ".png")))
        else:
            raise SystemExit("unknown action %r" % act)
    q.close()


if __name__ == "__main__":
    main()
