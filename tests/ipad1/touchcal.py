#!/usr/bin/env python3
"""touchcal.py: where do taps land? The multitouch profile's frame_* check for the portrait A4 boards.

Boots the device, opens Safari on a page served from the host whose touchstart handler draws a red mark at the
touch's page position, taps a 4x5 grid and finds each mark in the screendump. Every mark must sit within
--tolerance pixels of where the tap was aimed (pages report whole CSS pixels: 2 screen pixels on a Retina panel).

To recalibrate, fit mark = A + B * aimed per axis with the old frame values and solve: frame_width' = width / B;
x0' = x0 - A * width / (W * B) for x, and for y, whose sensor axis runs bottom-up on the N1,
y0' = y0 + height - height / B + A * height / (H * B) (hw/arm/ipod_touch_multitouch.c, mt_profile_n81).

  tests/ipad1/touchcal.py --machine iPhone-4 --device DEV --out DIR [--keyboard usb]
The URL is typed on the guest's own keyboard by default (taps, so a bad calibration fails early);
--keyboard usb types it on an emulated USB keyboard instead (N90 takes one; N81 4.2.1 does not).
"""
import argparse, http.server, os, sys, threading, time, urllib.parse
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import regress as R

PAGE = b"""<html><head><meta name="viewport" content="width=device-width, initial-scale=1, user-scalable=no"></head>
<body style="margin:0;height:2000px;background:#fff" ontouchstart="var t=event.touches[0];
var d=document.createElement('div');d.style.cssText='position:absolute;width:4px;height:4px;background:#f00;left:'+(t.pageX-2)+'px;top:'+(t.pageY-2)+'px';
document.body.appendChild(d);new Image().src='/t?x='+t.pageX+'&y='+t.pageY+'&r='+Math.random();event.preventDefault();">
<p>touchcal</p></body></html>"""
GRID = [(x, y) for y in (260, 400, 560, 700, 850) for x in (30, 200, 440, 610)]
# Safari 4.2.1, portrait 640x960: its first launch shows Bookmarks (Done), then the address field.
BOOKMARKS_DONE, ADDRESS, GO = (578, 83), (210, 113), (560, 915)
ROW1 = [32, 96, 160, 224, 288, 352, 416, 480, 544, 608]
KEYS = {c: (x, 590) for c, x in zip("qwertyuiop", ROW1)}
KEYS.update({c: (x, 698) for c, x in zip("asdfghjkl", [64, 128, 192, 256, 320, 384, 448, 512, 576])})
KEYS.update({c: (x, 805) for c, x in zip("zxcvbnm", [128, 192, 256, 320, 384, 448, 512])})
DIGITS = dict({c: (x, 590) for c, x in zip("1234567890", ROW1)}, **{":": (265, 805)})
MODE_KEY, DOT, SLASH = (80, 915), (213, 915), (320, 915)


def type_on_screen(b, text):
    mode = "abc"
    for ch in text:
        if ch in "./":
            b.tap(DOT if ch == "." else SLASH)
        else:
            want = "123" if ch in DIGITS else "abc"
            if mode != want:
                b.tap(MODE_KEY)
                time.sleep(0.6)
                mode = want
            b.tap(DIGITS[ch] if want == "123" else KEYS[ch])
        time.sleep(0.5)
    b.tap(GO)


def marks(path):
    w, h, pix = R.itqmp.read_ppm(path)
    clusters = []
    for y in range(h):
        for x in range(w):
            o = (y * w + x) * 3
            if pix[o] > 200 and pix[o + 1] < 60 and pix[o + 2] < 60:
                for c in clusters:
                    if abs(c[0][0] - x) < 12 and abs(c[0][1] - y) < 12:
                        c.append((x, y))
                        break
                else:
                    clusters.append([(x, y)])
    return sorted(((sum(p[0] for p in c) / len(c), sum(p[1] for p in c) / len(c)) for c in clusters),
                  key=lambda m: (round(m[1] / 20), m[0]))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    R.ipad1_boot.add_arguments(ap)
    ap.add_argument("--qemu", default=os.path.join(R.ROOT, "build/qemu-system-arm"))
    ap.add_argument("--usbmuxd", default=R.USBMUXD)
    ap.add_argument("--boot-timeout", type=int, default=600)
    ap.add_argument("--out", required=True)
    ap.add_argument("--keyboard", choices=("screen", "usb"), default="screen")
    ap.add_argument("--tolerance", type=float, default=2.0)
    for opt in ("--nand", "--product-version", "--guest-package"):
        ap.add_argument(opt)
    a = ap.parse_args()
    if a.machine not in R.ipad1_boot.PORTRAIT:
        sys.exit("touchcal knows the portrait Safari layout only")
    R.itqmp.W, R.itqmp.H = R.ipad1_boot.MACHINES[a.machine]
    R.UNLOCK_FROM, R.UNLOCK_TO, R.LIT_MIN_FRACTION = (116, 862), (600, 862), 0.2
    R.device_args(a)
    R.ipod.START = time.time()
    os.makedirs(a.out, exist_ok=True)
    hits = []

    class H(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            hits.append(self.path)
            body = PAGE if self.path.startswith("/cal") else b""
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, *x):
            pass

    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), H)   # slirp's 10.0.2.2 is host loopback
    threading.Thread(target=srv.serve_forever, daemon=True).start()

    class Res:
        def set(self, ok, detail):
            print("boot:", detail)

    b, detail = R.booted(a, "touchcal", Res(), keyboard=a.keyboard == "usb")
    try:
        if not detail:
            return 1
        t0 = time.time()
        while not R.itqmp.agent_alive(b.qmp) and time.time() - t0 < 120:
            time.sleep(3)
        b.press("home")
        time.sleep(1.5)
        b.drag(R.UNLOCK_FROM, R.UNLOCK_TO)
        time.sleep(3)
        R.itqmp.agent(b.qmp, "launch", "com.apple.mobilesafari", timeout=60)
        time.sleep(10)
        b.tap(BOOKMARKS_DONE)
        time.sleep(3)
        b.tap(ADDRESS)
        time.sleep(5)
        url = "10.0.2.2:%d/cal" % srv.server_address[1]
        if a.keyboard == "usb":
            b.type(url)
            b.qmp.cmd("send-key", keys=[{"type": "qcode", "data": "ret"}])
        else:
            type_on_screen(b, url)
        t0 = time.time()
        while not any(p.startswith("/cal") for p in hits) and time.time() - t0 < 60:
            time.sleep(1)
        if not any(p.startswith("/cal") for p in hits):
            print("FAIL Safari never loaded the page (shot: %s)" % b.shot("url"))
            return 1
        time.sleep(6)
        for p in GRID:
            n = len(hits)
            b.tap(p)
            t0 = time.time()
            while len(hits) == n and time.time() - t0 < 10:
                time.sleep(0.3)
            time.sleep(0.7)
        found = marks(b.shot("marks"))
        if len(found) != len(GRID):
            print("FAIL %d marks for %d taps" % (len(found), len(GRID)))
            return 1
        errs = [(m[0] - p[0], m[1] - p[1]) for p, m in zip(GRID, found)]
        worst = max(max(abs(dx), abs(dy)) for dx, dy in errs)
        print("%s %d taps, worst %.1f px off (x %s, y %s)" % (
            "PASS" if worst <= a.tolerance else "FAIL", len(GRID), worst,
            " ".join("%+.1f" % e[0] for e in errs[:4]), " ".join("%+.1f" % e[1] for e in errs[::4])))
        return 0 if worst <= a.tolerance else 1
    finally:
        b.stop()
        srv.shutdown()


if __name__ == "__main__":
    sys.exit(main())
