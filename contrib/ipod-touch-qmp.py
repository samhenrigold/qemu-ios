#!/usr/bin/env python3
"""
Minimal QMP driver for the emulated iPod touch: tap the touchscreen and grab
screenshots without needing a human at the SDL window.

  ipod-touch-qmp.py <port> shot <out.ppm>
  ipod-touch-qmp.py <port> tap <x> <y>          # 0..319 x, 0..479 y
  ipod-touch-qmp.py <port> swipe <x1> <y1> <x2> <y2>
  ipod-touch-qmp.py <port> key <qcode>
"""
import json
import socket
import sys
import time

W, H = 320, 480
ABS_MAX = 0x7fff


class QMP:
    def __init__(self, port):
        self.s = socket.create_connection(("127.0.0.1", port))
        self.f = self.s.makefile("rw", encoding="utf-8", newline="\n")
        self.f.readline()  # greeting
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
                raise ConnectionError("qmp closed")
            reply = json.loads(line)
            if "event" in reply:
                continue
            return reply

    def send_events(self, events):
        return self.cmd("input-send-event", events=events)

    def move(self, x, y):
        ax = int(x * ABS_MAX / (W - 1))
        ay = int(y * ABS_MAX / (H - 1))
        self.send_events([
            {"type": "abs", "data": {"axis": "x", "value": ax}},
            {"type": "abs", "data": {"axis": "y", "value": ay}},
        ])

    def tap(self, x, y, hold=0.12):
        self.move(x, y)
        time.sleep(0.05)
        self.send_events([{"type": "btn",
                           "data": {"down": True, "button": "left"}}])
        time.sleep(hold)
        self.send_events([{"type": "btn",
                           "data": {"down": False, "button": "left"}}])

    def swipe(self, x1, y1, x2, y2, steps=12):
        self.move(x1, y1)
        self.send_events([{"type": "btn",
                           "data": {"down": True, "button": "left"}}])
        for i in range(1, steps + 1):
            self.move(x1 + (x2 - x1) * i // steps,
                      y1 + (y2 - y1) * i // steps)
            time.sleep(0.02)
        self.send_events([{"type": "btn",
                           "data": {"down": False, "button": "left"}}])


def main():
    port = int(sys.argv[1])
    action = sys.argv[2]
    q = QMP(port)
    if action == "shot":
        print(q.cmd("screendump", filename=sys.argv[3]))
    elif action == "tap":
        q.tap(int(sys.argv[3]), int(sys.argv[4]))
        print("tapped")
    elif action == "swipe":
        q.swipe(*(int(a) for a in sys.argv[3:7]))
        print("swiped")
    elif action == "key":
        q.send_events([{"type": "key",
                        "data": {"down": True,
                                 "key": {"type": "qcode",
                                         "data": sys.argv[3]}}}])
        time.sleep(0.1)
        q.send_events([{"type": "key",
                        "data": {"down": False,
                                 "key": {"type": "qcode",
                                         "data": sys.argv[3]}}}])
        print("key sent")
    else:
        print(__doc__)


if __name__ == "__main__":
    main()
