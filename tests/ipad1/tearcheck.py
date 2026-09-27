#!/usr/bin/env python3
"""Measure tearing and black/partial frames on the iPad 1 during animations.

    tests/ipad1/tearcheck.py [--qemu build/qemu-system-arm] [--out DIR]
    tests/ipad1/tearcheck.py --analyze DIR       # re-score a previous capture

Restores checkpoint-lock (via boot-smoke.py --from-checkpoint --keep), then
runs a fixed scenario: wake + unlock, home-screen page swipe and back, launch
Notes, Home. A second thread screendumps back to back the whole time. That
goes through the same display_update() read of the guest framebuffer the app
publishes at its refresh, so what tears here tears in the app.

Scoring, per captured frame i against its neighbours i-1 and i+1, row by row
(panel rows are framebuffer rows, the order the guest writes memory in):
  torn     bands of >= BAND rows equal to frame i-1 only, and bands of
           >= BAND rows equal to frame i+1 only, on opposite sides of one
           row: the frame is one state above it and the next below.
           Interleaved bands are content moving across rows (the page
           swipe, rotated onto the landscape panel), not a tear.
  black    every pixel zero while both neighbours have content.
  partial  a band of >= BLACK_BAND fully black rows where both neighbours
           have content in those rows.
Frames identical to both neighbours are idle and not counted. Output:
summary.json in DIR, plus worst-N.png (prev | frame | next, bands marked).
"""
import argparse, glob, json, os, re, subprocess, sys, threading, time

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, f"{ROOT}/imgtools")
FILES = os.path.expanduser("~/Developer/qemu-ios-files/ipad1")
BAND, BLACK_BAND = 8, 32
W, H = 1024, 768          # landscape panel; the portrait UI is rotated onto it


def read_ppm(path):
    d = open(path, "rb").read()
    m = re.match(rb"P6\s+(\d+)\s+(\d+)\s+(\d+)\s", d)
    w, h = int(m.group(1)), int(m.group(2))
    return np.frombuffer(d, np.uint8, w * h * 3, m.end()).reshape(h, w, 3)


def runs(mask):
    """(start, length) of each run of True."""
    out, start = [], None
    for i, v in enumerate(list(mask) + [False]):
        if v and start is None:
            start = i
        elif not v and start is not None:
            out.append((start, i - start))
            start = None
    return out


def score(frames):
    res = []
    for i in range(1, len(frames) - 1):
        a, f, b = frames[i - 1], frames[i], frames[i + 1]
        eq_prev = (f == a).all(axis=(1, 2))
        eq_next = (f == b).all(axis=(1, 2))
        if eq_prev.all() and eq_next.all():
            continue
        r = {"i": i, "kind": []}
        only_prev = [x for x in runs(eq_prev & ~eq_next) if x[1] >= BAND]
        only_next = [x for x in runs(eq_next & ~eq_prev) if x[1] >= BAND]
        # A tear is one frame above a row and the next below it (the guest
        # writes rows in order). Bands that interleave are motion across
        # rows (the rotated home-screen page swipe moves icons down the
        # panel's rows), not a tear.
        split = only_prev and only_next and (
            max(y + n for y, n in only_prev) <= min(y for y, n in only_next) or
            max(y + n for y, n in only_next) <= min(y for y, n in only_prev))
        if split:
            r["kind"].append("torn")
            r["bands"] = only_prev + only_next
            r["severity"] = min(max(x[1] for x in only_prev), max(x[1] for x in only_next))
        blank = ~f.any(axis=(1, 2))
        lit_a, lit_b = a.any(axis=(1, 2)), b.any(axis=(1, 2))
        if blank.all() and lit_a.any() and lit_b.any():
            r["kind"].append("black")
            r["severity"] = H
        else:
            holes = [x for x in runs(blank & lit_a & lit_b) if x[1] >= BLACK_BAND]
            if holes:
                r["kind"].append("partial")
                r["bands"] = r.get("bands", []) + holes
                r["severity"] = max(r.get("severity", 0), max(x[1] for x in holes))
        r["changed"] = True
        res.append(r)
    return res


def save_worst(frames, res, out, n=6):
    from PIL import Image, ImageDraw
    bad = sorted((r for r in res if r["kind"]), key=lambda r: -r["severity"])[:n]
    for k, r in enumerate(bad):
        i = r["i"]
        strip = np.concatenate([frames[i - 1], frames[i], frames[i + 1]], axis=1)
        im = Image.fromarray(strip)
        d = ImageDraw.Draw(im)
        for y, ln in r.get("bands", []):
            d.rectangle([W, y, 2 * W - 1, y + ln - 1], outline=(255, 0, 255), width=3)
        im.resize((im.width // 2, im.height // 2)).save(f"{out}/worst-{k + 1}-frame{i}-{'+'.join(r['kind'])}.png")


def analyze(out):
    paths = sorted(glob.glob(f"{out}/f*.ppm"))
    frames = [read_ppm(p) for p in paths]
    times = json.load(open(f"{out}/times.json")) if os.path.exists(f"{out}/times.json") else []
    res = score(frames)
    changed = len(res)
    count = lambda k: sum(k in r["kind"] for r in res)
    summary = {
        "samples": len(times), "distinct_frames": len(frames),
        "capture_fps": round((len(times) - 1) / (times[-1] - times[0]), 1) if len(times) > 1 else None,
        "changed_frames": changed,
        "torn": count("torn"), "black": count("black"), "partial": count("partial"),
        "tear_rate": round(count("torn") / changed, 3) if changed else 0.0,
        "bad_rate": round(sum(bool(r["kind"]) for r in res) / changed, 3) if changed else 0.0,
        "worst": [{k: r[k] for k in ("i", "kind", "severity")} for r in
                  sorted((r for r in res if r["kind"]), key=lambda r: -r["severity"])[:6]],
    }
    json.dump(summary, open(f"{out}/summary.json", "w"), indent=1)
    save_worst(frames, res, out)
    return summary


# ---- capture ---------------------------------------------------------------

class Rig:
    def __init__(self, qmp_path):
        from itqmp import QMP
        self.q, self.lock = QMP(qmp_path, timeout=30), threading.Lock()

    def cmd(self, name, **args):
        with self.lock:
            return self.q.cmd(name, **args)

    def ev(self, x=None, y=None, down=None):
        e = []
        if x is not None:
            e += [{"type": "abs", "data": {"axis": "x", "value": int(x * 32767 / W)}},
                  {"type": "abs", "data": {"axis": "y", "value": int(y * 32767 / H)}}]
        if down is not None:
            e += [{"type": "btn", "data": {"button": "left", "down": down}}]
        self.cmd("input-send-event", events=e)

    def drag(self, x0, y0, x1, y1, steps=20, dt=0.03):
        self.ev(x0, y0)
        self.ev(down=True)
        time.sleep(0.12)
        for i in range(1, steps + 1):
            self.ev(x0 + (x1 - x0) * i / steps, y0 + (y1 - y0) * i / steps)
            time.sleep(dt)
        self.ev(down=False)

    def tap(self, x, y):
        self.ev(x, y)
        self.ev(down=True)
        time.sleep(0.12)
        self.ev(down=False)

    def button(self, name, hold=0.2):
        self.cmd("qom-set", path="/machine", property=name, value=True)
        time.sleep(hold)
        self.cmd("qom-set", path="/machine", property=name, value=False)


def scenario(rig):
    rig.button("button-home")                 # the checkpoint's lock screen may have blanked
    time.sleep(1.5)
    rig.drag(959, 477, 959, 57)                # slide to unlock (knob on the panel's right edge)
    time.sleep(3)
    rig.tap(608, 382)                          # first-unlock "Edit Home Screen" tip: Dismiss
    time.sleep(1.5)
    rig.drag(523, 167, 523, 617)               # page swipe (portrait x runs along panel y)
    time.sleep(2)
    rig.drag(523, 617, 523, 167)               # and back
    time.sleep(2)
    rig.tap(128, 297)                          # Notes
    time.sleep(4)
    rig.button("button-home")
    time.sleep(3)


def cold_boot(a):
    """--boot STORE: stores that cannot be checkpointed (live host GL state,
    i.e. accelerated CoreAnimation) boot from scratch on a fresh overlay and
    run the scenario once SpringBoard is up."""
    import tempfile
    td = tempfile.mkdtemp(prefix="tc-", dir="/tmp")
    qmp_path = f"{td}/qmp"
    os.mkdir(f"{td}/overlay")          # the IOP won't create the overlay directory itself
    child = subprocess.Popen([a.qemu, "-machine",
                              f"ipad1,kboot={FILES}/7B500/k48-kboot.bin,nand={a.boot},nand-overlay={td}/overlay",
                              "-display", "none", "-monitor", "none", "-serial", f"file:{td}/serial.log",
                              "-qmp", f"unix:{qmp_path},server=on,wait=off"],
                             stdout=subprocess.DEVNULL, stderr=open(f"{td}/stderr", "w"),
                             start_new_session=True)
    while not os.path.exists(qmp_path):
        if child.poll() is not None:
            raise SystemExit(open(f"{td}/stderr").read())
        time.sleep(0.1)
    from itqmp import QMP
    # SpringBoard's lock screen is up well within 150 s (the timing
    # tests/ipad1/gl-drive.py uses); the scenario's Home press wakes it.
    q = QMP(qmp_path, timeout=30)
    time.sleep(150)
    q.close()
    return child.pid, qmp_path


def capture(a):
    os.makedirs(a.out, exist_ok=True)
    for p in glob.glob(f"{a.out}/*"):
        os.unlink(p)
    a.out = os.path.abspath(a.out)
    if a.boot:
        pid, qmp_path = cold_boot(a)
    else:
        boot = subprocess.run([sys.executable, f"{ROOT}/tests/ipad1/boot-smoke.py", "--from-checkpoint",
                               a.checkpoint, "--keep", "--qemu", a.qemu, "--seconds", "120"],
                              capture_output=True, text=True)
        m = re.search(r"QEMU pid (\d+) left running; QMP at (\S+)", boot.stdout)
        if not m:
            raise SystemExit("checkpoint restore failed:\n" + boot.stdout + boot.stderr)
        pid, qmp_path = int(m.group(1)), m.group(2)
    rig = Rig(qmp_path)
    times, stop = [], threading.Event()

    def grab():
        # Paced like the app's refresh: back-to-back dumps hold the BQL so
        # long the guest barely runs. Only frames that differ from the last
        # kept one are written; `times` counts every sample.
        n, last, tmp = 0, None, f"{a.out}/cur.ppm"
        while not stop.is_set():
            t0 = time.monotonic()
            rig.cmd("screendump", filename=tmp)
            times.append(t0)
            data = open(tmp, "rb").read()
            if data != last:
                os.replace(tmp, f"{a.out}/f{n:05d}.ppm")
                last, n = data, n + 1
            time.sleep(max(0.0, 1 / a.fps - (time.monotonic() - t0)))
    t = threading.Thread(target=grab)
    try:
        t.start()
        scenario(rig)
    finally:
        stop.set()
        t.join()
        json.dump(times, open(f"{a.out}/times.json", "w"))
        if os.path.exists(f"{a.out}/cur.ppm"):
            os.unlink(f"{a.out}/cur.ppm")
        try:
            rig.cmd("quit")
        except Exception:
            pass
        time.sleep(1)
        try:
            os.kill(pid, 9)
        except ProcessLookupError:
            pass


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--qemu", default=f"{ROOT}/build/qemu-system-arm")
    ap.add_argument("--checkpoint", default=f"{FILES}/userland/checkpoint-lock")
    ap.add_argument("--boot", metavar="STORE", help="cold-boot STORE (read-only base, fresh overlay) instead of restoring a checkpoint")
    ap.add_argument("--out", default="/tmp/tearcheck")
    ap.add_argument("--fps", type=float, default=60, help="sample rate (the app refreshes at ~60 Hz)")
    ap.add_argument("--analyze", metavar="DIR", help="score an existing capture only")
    a = ap.parse_args()
    if a.analyze:
        a.out = a.analyze
    else:
        capture(a)
    s = analyze(a.out)
    print(json.dumps({k: v for k, v in s.items() if k != "worst"}))
    print(f"worst frames: {a.out}/worst-*.png")


if __name__ == "__main__":
    main()
