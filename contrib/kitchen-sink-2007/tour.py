#!/usr/bin/env python3
"""tour.py -- boot an M68 with Kitchen.app sideloaded, visit every screen and screendump each one.

    tour.py --device DEV --overlay OVL --qemu QEMU --out DIR [--frames]

DEV is an M68 device directory (nand/, nor.bin, iBoot.bin); OVL an overlay that imgtools/sideload1x.py put
Kitchen.app into. Taps and drags go through QMP input events, as a finger would. Writes DIR/NN-name.png per
screen (and the interactions on it), DIR/contact-sheet.png, and with --frames DIR/tour.gif (ffmpeg) of the
whole visit. One QEMU; it exits when the tour ends.
"""
import argparse, json, os, shutil, socket, subprocess, sys, tempfile, time

BOOTROM = os.path.expanduser("~/Developer/qemu-ios-files/ipod1g/bootrom_s5l8900")
ROW = lambda i: 87 + 46 * i               # root list row centers (status 20 + bar 44 + 46-point rows)
BACK = (32, 42)
SCROLLED = 274                            # 15 rows of 46 in a 416-point table, scrolled to the end
TAB = lambda i: 32 + 64 * i               # button bar buttons


class Device:
    def __init__(self, a, run):
        self.run, self.frames, self.n = run, a.frames, 0
        shutil.copy(os.path.join(a.device, "nor.bin"), os.path.join(run, "nor.bin"))
        os.chmod(os.path.join(run, "nor.bin"), 0o644)
        sock = os.path.join(run, "q.sock")
        m = "iPhone-2G,bootrom=%s,iboot=%s,nand=%s,nand-overlay=%s" % (
            BOOTROM, os.path.join(a.device, "iBoot.bin"), os.path.join(a.device, "nand"), a.overlay)
        self.p = subprocess.Popen([a.qemu, "-M", m, "-drive", "if=pflash,format=raw,file=%s/nor.bin" % run,
                                   "-serial", "file:%s/serial.log" % run, "-display", "none", "-audio", "driver=none",
                                   "-qmp", "unix:%s,server,nowait" % sock],
                                  stdout=open(os.path.join(run, "qemu.log"), "w"), stderr=subprocess.STDOUT,
                                  stdin=subprocess.DEVNULL)
        for _ in range(100):
            try:
                s = socket.socket(socket.AF_UNIX)
                s.connect(sock)
                break
            except OSError:
                time.sleep(0.1)
        self.f = s.makefile("rw")
        self.f.readline()
        self.cmd("qmp_capabilities")

    def cmd(self, c, args=None):
        self.f.write(json.dumps({"execute": c, "arguments": args or {}}) + "\n")
        self.f.flush()
        while True:
            r = json.loads(self.f.readline())
            if "event" not in r:
                return r

    def move(self, x, y):
        self.cmd("input-send-event", {"events": [{"type": "abs", "data": {"axis": "x", "value": int(x * 0x7fff / 319)}},
                                                 {"type": "abs", "data": {"axis": "y", "value": int(y * 0x7fff / 479)}}]})

    def button(self, down):
        self.cmd("input-send-event", {"events": [{"type": "btn", "data": {"down": down, "button": "left"}}]})

    def tap(self, x, y):
        self.move(x, y)
        time.sleep(0.05)
        self.button(True)
        time.sleep(0.15)
        self.button(False)

    def drag(self, x1, y1, x2, y2, steps=16):
        self.move(x1, y1)
        self.button(True)
        time.sleep(0.2)
        for i in range(1, steps + 1):
            self.move(x1 + (x2 - x1) * i / steps, y1 + (y2 - y1) * i / steps)
            time.sleep(0.06)
        time.sleep(0.2)
        self.button(False)

    def dump(self, path):
        self.cmd("screendump", {"filename": path})

    def wait(self, seconds):
        end = time.time() + seconds
        while time.time() < end:
            if self.frames:
                self.dump(os.path.join(self.frames, "f%05d.ppm" % self.n))
                self.n += 1
            time.sleep(0.25)


def tour(d, out):
    shots = []

    def shot(name):
        ppm = os.path.join(d.run, "s.ppm")
        d.dump(ppm)
        png = os.path.join(out, "%02d-%s.png" % (len(shots), name))
        subprocess.run(["sips", "-s", "format", "png", ppm, "--out", png], capture_output=True)
        shots.append(png)
        print("shot", png, flush=True)

    d.wait(62)                            # boot to the home screen
    d.tap(160, 268)                       # dismiss "iPhone is activated"
    d.wait(2)
    d.tap(38, 340)                        # Kitchen
    d.wait(14)
    shot("root")
    offset = 0
    # Points are screen coordinates: the app's views start at y 64, under the status bar and navigation bar.
    screens = [
        ("buttons", [("tap", 85, 122), ("wait", 1), ("tap", 160, 250), ("tap", 40, 372), ("wait", 1),
                     ("shot", "tapped")]),
        ("controls", [("tap", 253, 132), ("drag", 136, 178, 230, 178), ("tap", 160, 404), ("wait", 1),
                      ("shot", "changed")]),
        ("text", [("tap", 176, 315), ("tap", 240, 261), ("wait", 1), ("shot", "typed")]),
        ("alerts", [("tap", 160, 106), ("wait", 1.5), ("shot", "alert"), ("tap", 160, 261), ("wait", 1),
                    ("tap", 160, 160), ("wait", 1.5), ("shot", "sheet"), ("tap", 160, 370), ("wait", 1),
                    ("tap", 160, 214), ("wait", 1.5), ("shot", "textfield"), ("tap", 160, 128), ("wait", 1),
                    ("tap", 160, 268), ("wait", 1.5), ("shot", "hud"), ("wait", 4)]),
        ("progress", [("wait", 2), ("shot", "later")]),
        ("pickers", [("drag", 100, 250, 100, 170), ("wait", 2), ("shot", "spun"), ("tap", 160, 87), ("wait", 1.5),
                     ("shot", "date"), ("tap", 280, 87), ("wait", 1.5), ("shot", "timer")]),
        ("scroller", [("drag", 250, 380, 80, 120), ("wait", 1.5), ("shot", "scrolled")]),
        ("prefs", [("drag", 160, 400, 160, 120), ("wait", 1.5), ("shot", "scrolled")]),
        ("sections", [("tap", 312, 300), ("wait", 1.5), ("shot", "index")]),
        ("transitions", [("tap", 160, 338), ("wait", 0.3), ("shot", "moving"), ("wait", 1), ("tap", 160, 338),
                         ("wait", 1.2), ("tap", 160, 338), ("wait", 1.2), ("shot", "card4")]),
        ("animator", [("tap", 160, 286), ("wait", 0.4), ("shot", "spinning"), ("wait", 1), ("tap", 257, 286),
                      ("tap", 63, 286), ("wait", 1), ("shot", "moved")]),
        ("navbars", [("tap", 290, 100), ("wait", 1), ("shot", "styled")]),
        ("buttonbar", [("tap", TAB(2), 425), ("wait", 1), ("shot", "contacts")]),
        ("web", [("wait", 3), ("shot", "loaded")]),
        ("labels", [("wait", 2), ("shot", "later")]),
    ]
    for i, (name, steps) in enumerate(screens):
        if i == 9 and not offset:
            d.drag(160, 440, 160, 70)     # bring rows 9-14 up
            d.wait(2)
            offset = SCROLLED
            shot("root-scrolled")
        d.tap(160, ROW(i) - offset)
        d.wait(2)
        shot(name)
        for s in steps:
            if s[0] == "tap":
                d.tap(s[1], s[2])
            elif s[0] == "drag":
                d.drag(*s[1:])
            elif s[0] == "wait":
                d.wait(s[1])
            else:
                shot("%s-%s" % (name, s[1]))
        d.tap(*BACK)
        d.wait(1.5)
    shot("root-again")
    return shots


def contact_sheet(shots, out):
    from PIL import Image, ImageDraw, ImageFont
    cols, w, h, pad = 6, 200, 300, 26
    rows = (len(shots) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * (w + 10) + 10, rows * (h + pad + 10) + 10), (24, 28, 34))
    d = ImageDraw.Draw(sheet)
    try:
        font = ImageFont.truetype("/System/Library/Fonts/Helvetica.ttc", 13)
    except OSError:
        font = ImageFont.load_default()
    for k, p in enumerate(shots):
        x, y = 10 + (k % cols) * (w + 10), 10 + (k // cols) * (h + pad + 10)
        sheet.paste(Image.open(p).convert("RGB").resize((w, h)), (x, y))
        d.text((x + w / 2, y + h + 13), os.path.basename(p)[:-4], font=font, fill=(220, 225, 235), anchor="mm")
    sheet.save(os.path.join(out, "contact-sheet.png"))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--device", required=True)
    ap.add_argument("--overlay", required=True)
    ap.add_argument("--qemu", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--frames", action="store_true", help="also record the visit as tour.gif")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    run = tempfile.mkdtemp(prefix="ks-tour.")
    a.frames = os.path.join(run, "frames") if a.frames else None
    # Boot a private copy: the tour ends by stopping QEMU, and a guest stopped that way leaves its overlay
    # unclean (the next boot hangs at the logo, and sideload1x.py refuses it).
    shutil.copytree(a.overlay, os.path.join(run, "ovl"))
    a.overlay = os.path.join(run, "ovl")
    if a.frames:
        os.makedirs(a.frames)
    d = Device(a, run)
    try:
        shots = tour(d, a.out)
    finally:
        d.p.terminate()
        d.p.wait(10)
    contact_sheet(shots, a.out)
    if a.frames:
        subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-framerate", "8", "-i",
                        os.path.join(a.frames, "f%05d.ppm"), "-vf",
                        "fps=8,scale=240:-1:flags=lanczos,split[s0][s1];[s0]palettegen[p];[s1][p]paletteuse",
                        os.path.join(a.out, "tour.gif")], check=True)
    shutil.rmtree(run)
    print("contact sheet:", os.path.join(a.out, "contact-sheet.png"))


if __name__ == "__main__":
    sys.exit(main())
