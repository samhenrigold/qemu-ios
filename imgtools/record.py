#!/usr/bin/env python3
"""Record the emulator's screen to a video file, over QMP.

    imgtools/record.py --qmp 4444 --out demo.mp4
    imgtools/record.py --qmp 4444 --out demo.mp4 --fps 15 --duration 30
    imgtools/record.py --qmp 4444 --out demo.gif

Stop with Ctrl-C; the file is finalised on the way out.

WHY THIS AND NOT SOMETHING IN QEMU
----------------------------------
QEMU has no video capture path of its own -- the nearest things are a VNC server
you would then have to record, or a display backend, and both mean a window or a
second protocol. `screendump` already exists, already works headless, and is far
cheaper than it looks: measured at 287 screendumps per second on a loaded host
(load average 24), which is ~5x the panel's own 60 Hz refresh and nearly 20x the
30 fps this defaults to. So the whole recorder is a screendump loop feeding
ffmpeg, and it costs the emulator nothing worth measuring.

The frames are taken on wall-clock time, not guest time. A guest that stalls
records as a freeze, which is what you want when the freeze is the thing you are
recording.

ONE QMP SOCKET IS NOT ENOUGH. `-qmp tcp:...,server,nowait` accepts a single
client, and this holds it for the whole recording -- so itdrive.py cannot get in
to tap anything while you record, and its connection just times out. Give QEMU a
second monitor to record through:

    -qmp tcp:127.0.0.1:4444,server,nowait -qmp tcp:127.0.0.1:4445,server,nowait

BRIGHTNESS
----------
The panel scales every pixel by the backlight the guest programmed, so a
screendump of a dim screen is faithful and illegible. --gain auto (the default)
measures the brightest sub-pixel over the first second and then HOLDS that
scale for the whole recording: computing it per frame instead makes the exposure
pump every time something bright leaves the screen, which looks far worse than
a dark video. --gain 1 disables it, --gain <n> forces a factor.

ROTATION
--------
The console resizes when the guest rotates, and a video cannot change size
half way through. The first frame fixes the canvas and later frames are centred
into it, so a recording that starts in portrait shows a landscape guest
letterboxed rather than ending at the rotation.
"""

import argparse
import json
import os
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time


class QMP:
    def __init__(self, host, port, timeout=10):
        self.s = socket.create_connection((host, port), timeout)
        self.f = self.s.makefile("rwb")
        self.f.readline()                 # greeting, which carries no "return"
        self.cmd("qmp_capabilities")

    def _read(self):
        while True:
            line = self.f.readline()
            if not line:
                raise RuntimeError("QMP closed the connection")
            msg = json.loads(line)
            if "return" in msg or "error" in msg:
                return msg

    def cmd(self, name, **args):
        self.f.write(json.dumps({"execute": name, "arguments": args}).encode()
                     + b"\n")
        self.f.flush()
        msg = self._read()
        if "error" in msg:
            raise RuntimeError("%s: %s" % (name, msg["error"]["desc"]))
        return msg["return"]


def read_ppm(path):
    """(w, h, rgb bytes) from a binary PPM."""
    with open(path, "rb") as f:
        data = f.read()
    parts, i = [], 0
    while len(parts) < 4:
        while data[i:i + 1].isspace():
            i += 1
        if data[i:i + 1] == b"#":
            while data[i:i + 1] != b"\n":
                i += 1
            continue
        start = i
        while not data[i:i + 1].isspace():
            i += 1
        parts.append(data[start:i])
    i += 1
    w, h = int(parts[1]), int(parts[2])
    return w, h, data[i:i + w * h * 3]


def fit(pix, w, h, cw, ch):
    """Centre a w x h frame into a cw x ch black canvas."""
    if (w, h) == (cw, ch):
        return pix
    out = bytearray(cw * ch * 3)
    x0 = max(0, (cw - w) // 2)
    y0 = max(0, (ch - h) // 2)
    cols = min(w, cw)
    for y in range(min(h, ch)):
        src = (y * w) * 3
        dst = ((y + y0) * cw + x0) * 3
        out[dst:dst + cols * 3] = pix[src:src + cols * 3]
    return bytes(out)


def main():
    ap = argparse.ArgumentParser(
        description="record the emulator's screen over QMP",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__)
    ap.add_argument("--qmp", required=True,
                    help="QMP port, or host:port")
    ap.add_argument("--out", required=True, help="output file (.mp4, .gif, ...)")
    ap.add_argument("--fps", type=int, default=30)
    ap.add_argument("--duration", type=float, default=0,
                    help="seconds; 0 means until Ctrl-C")
    ap.add_argument("--gain", default="auto",
                    help="auto (default), 1 to disable, or a factor")
    args = ap.parse_args()

    if not shutil.which("ffmpeg"):
        sys.exit("record.py: ffmpeg is not on PATH")

    host, _, port = args.qmp.rpartition(":")
    q = QMP(host or "127.0.0.1", int(port))

    tmpdir = tempfile.mkdtemp(prefix="itrecord-")
    shot = os.path.join(tmpdir, "frame.ppm")

    # First frame fixes the canvas, and tells us whether there is anything there
    # at all -- starting ffmpeg on a console that has not been sized yet writes
    # a zero-byte video and reports success.
    q.cmd("screendump", filename=shot)
    cw, ch, first = read_ppm(shot)
    if not cw or not ch:
        sys.exit("record.py: the console has no size yet")

    gain = None if args.gain == "auto" else float(args.gain)
    gain_frames = args.fps          # hold the scale after one second
    lut = None
    applied = None

    ff = subprocess.Popen(
        ["ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
         "-f", "rawvideo", "-pix_fmt", "rgb24",
         "-s", "%dx%d" % (cw, ch), "-framerate", str(args.fps),
         "-i", "-",
         # yuv420p and even dimensions, or nothing will play it back
         "-vf", "scale=trunc(iw/2)*2:trunc(ih/2)*2",
         "-pix_fmt", "yuv420p", args.out],
        stdin=subprocess.PIPE)

    stop = {"now": False}
    signal.signal(signal.SIGINT, lambda *_: stop.__setitem__("now", True))
    signal.signal(signal.SIGTERM, lambda *_: stop.__setitem__("now", True))

    period = 1.0 / args.fps
    start = time.time()
    n = 0
    peak = 0
    try:
        while not stop["now"]:
            if args.duration and time.time() - start >= args.duration:
                break
            due = start + n * period
            now = time.time()
            if due > now:
                time.sleep(due - now)

            q.cmd("screendump", filename=shot)
            w, h, pix = read_ppm(shot)

            if gain is None:
                peak = max(peak, max(pix) if pix else 0)
                cur = 255.0 / peak if 0 < peak < 255 else 1.0
                # Lock the scale after a second, but only once there has been
                # something to measure. Recordings routinely start on a black
                # screen -- a boot, a locked device -- and a gain locked from
                # that is locked from no information at all.
                if n + 1 >= gain_frames and peak > 16:
                    gain = cur
            else:
                cur = gain

            if cur != 1.0:
                if cur != applied:
                    lut = bytes(min(255, int(v * cur)) for v in range(256))
                    applied = cur
                pix = pix.translate(lut)

            ff.stdin.write(fit(pix, w, h, cw, ch))
            n += 1
    finally:
        try:
            ff.stdin.close()
        except Exception:
            pass
        ff.wait()
        shutil.rmtree(tmpdir, ignore_errors=True)

    took = time.time() - start
    print("record.py: %d frames in %.1fs (%.1f fps) -> %s%s"
          % (n, took, n / took if took else 0, args.out,
             "" if (gain or 1.0) == 1.0 else "  [gain %.2f]" % gain))


main()
