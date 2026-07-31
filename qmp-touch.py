#!/usr/bin/env python3
"""Drive the emulated touchscreen and take screenshots over QMP.

    qmp-touch.py <port> tap <x> <y>
    qmp-touch.py <port> swipe <x1> <y1> <x2> <y2>
    qmp-touch.py <port> shot <path>

Coordinates are in screen pixels (320x480); QEMU's absolute axes are
0..32767, so they are scaled here.
"""
import json
import socket
import sys
import time

W, H = 320, 480


class Qmp:
    def __init__(self, port):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=15)
        self.f = self.s.makefile("rw")
        self.f.readline()
        self.cmd("qmp_capabilities")

    def cmd(self, execute, **args):
        msg = {"execute": execute}
        if args:
            msg["arguments"] = args
        self.f.write(json.dumps(msg) + "\n")
        self.f.flush()
        while True:
            line = self.f.readline()
            if not line:
                raise RuntimeError("qmp closed")
            r = json.loads(line)
            if "event" in r:
                continue
            return r

    def move(self, x, y):
        ax = int(x * 32767 / W)
        ay = int(y * 32767 / H)
        return self.cmd("input-send-event", events=[
            {"type": "abs", "data": {"axis": "x", "value": ax}},
            {"type": "abs", "data": {"axis": "y", "value": ay}},
        ])

    def btn(self, down):
        return self.cmd("input-send-event", events=[
            {"type": "btn", "data": {"down": down, "button": "left"}},
        ])


def main():
    port = int(sys.argv[1])
    op = sys.argv[2]
    q = Qmp(port)
    if op == "tap":
        x, y = int(sys.argv[3]), int(sys.argv[4])
        q.move(x, y)
        time.sleep(0.1)
        q.btn(True)
        time.sleep(0.25)
        q.btn(False)
        print("tap %d,%d" % (x, y))
    elif op == "swipe":
        x1, y1, x2, y2 = (int(v) for v in sys.argv[3:7])
        q.move(x1, y1)
        time.sleep(0.1)
        q.btn(True)
        steps = 12
        for i in range(1, steps + 1):
            q.move(x1 + (x2 - x1) * i // steps, y1 + (y2 - y1) * i // steps)
            time.sleep(0.04)
        time.sleep(0.1)
        q.btn(False)
        print("swipe %d,%d -> %d,%d" % (x1, y1, x2, y2))
    elif op == "shot":
        print(q.cmd("screendump", filename=sys.argv[3]))
    else:
        raise SystemExit("unknown op " + op)


if __name__ == "__main__":
    main()
