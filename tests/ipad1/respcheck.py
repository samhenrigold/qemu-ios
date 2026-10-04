#!/usr/bin/env python3
"""iPad 1 responsiveness: lock-screen-to-touch gap and touch-to-photon latency.

    tests/ipad1/respcheck.py [--qemu build/qemu-system-arm] [--out DIR] [--device DIR]

Cold-boots golden-pristine through a fresh copy-on-write overlay, then:
  1. Polls the screen until the lock screen is lit, and from then on probes
     slide-to-unlock every PROBE s: press the knob, drag it partway, dump the
     screen, and check whether the knob moved. The drag is taken back before
     release so the probe does not unlock. The gap between "lit" and "knob
     tracks" is what feels like a frozen lock screen.
  2. Samples the guest PC at ~20 Hz (HMP `info registers`) the whole time and
     buckets it: kernel kext / kernel symbol, dyld shared cache library and
     symbol, or the main executable. The histogram for the gap window says what
     the guest is doing while it ignores the finger.
  3. Unlocks, opens Settings, and times touch-down to the first frame where the
     touched row's highlight shows, over several rows. Frames are polled at
     ~60 Hz, the app's refresh.

Writes result.json (probes, tap_at_s, histograms) and pc-samples.json to
DIR and prints a summary.
"""
import argparse, bisect, json, os, re, statistics, subprocess, sys, tempfile, threading, time

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, f"{ROOT}/imgtools")
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from tearcheck import Rig, read_ppm, W, H          # noqa: E402

FILES = os.path.expanduser("~/Developer/qemu-ios-files/ipad1")
PROBE = 0.5
KNOB = (slice(913, 1003), slice(47, 517))           # panel x, y of the slider track
ROWS = [(305, 617), (436, 617), (348, 617), (393, 617)]   # Settings: Safari, Photos, iPod, Video


class Symbols:
    def __init__(self):
        self.kexts = []
        for line in open(f"{FILES}/7B500/work/kexts.txt"):
            p = line.split()
            if len(p) >= 4 and p[0].startswith("0x"):
                self.kexts.append((int(p[0], 16), int(p[0], 16) + int(p[1], 16), p[3].split(".")[-1]))
        ks = []
        for line in open(f"{FILES}/7B500/work/ksyms.txt"):
            m = re.search(r"0x([0-9a-f]+).*?\x1b\[1m(\S+?)\x1b", line)
            if m:
                ks.append((int(m.group(1), 16), m.group(2)))
        ks.sort()
        self.kaddr, self.kname = [a for a, _ in ks], [n for _, n in ks]
        us = {}
        for line in open(f"{FILES}/7B500/dsc-symbols.tsv"):
            p = line.rstrip("\n").split("\t")
            if len(p) == 3 and p[0].startswith("0x"):
                us[int(p[0], 16)] = (p[1], p[2])
        self.uaddr = sorted(us)
        self.uname = [us[a] for a in self.uaddr]

    def name(self, pc, kernel):
        if kernel:
            for lo, hi, n in self.kexts:
                if lo <= pc < hi:
                    return f"kext:{n}"
            i = bisect.bisect_right(self.kaddr, pc) - 1
            return f"kernel:{self.kname[i]}" if i >= 0 and pc - self.kaddr[i] < 0x4000 else "kernel:?"
        if 0x30000000 <= pc < 0x38000000:
            i = bisect.bisect_right(self.uaddr, pc) - 1
            if i >= 0:
                sym, lib = self.uname[i]
                return f"{lib}:{sym}" if pc - self.uaddr[i] < 0x4000 else f"{lib}:?"
        return "user:main-executable" if pc < 0x30000000 else "user:?"


def lit(frame):
    return frame.max() > 40 and frame.mean() > 8


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--qemu", default=f"{ROOT}/build/qemu-system-arm")
    import ipad1_boot
    ipad1_boot.add_arguments(ap)
    ap.add_argument("--base", help="override selected device NAND")
    ap.add_argument("--out", default="/tmp/respcheck")
    ap.add_argument("--samples", type=int, default=8, help="Settings row taps to time")
    ap.add_argument("--first-probe-delay", type=float, default=0.0,
                    help="wait this long after the lock screen lights before the first probe")
    a = ap.parse_args()
    a.base = a.base or os.path.join(a.device, "nand")
    os.makedirs(a.out, exist_ok=True)
    td = tempfile.mkdtemp(prefix="resp-", dir="/tmp")
    qmp = f"{td}/qmp"
    os.mkdir(f"{td}/overlay")
    lock = os.path.join(os.path.dirname(os.path.abspath(a.base)), "device.lock.json")
    v4 = os.path.exists(lock) and json.load(open(lock)).get("product_version", "").startswith("4.")
    machine = f"ipad1,{ipad1_boot.boot_options(a, td)},nand={a.base},nand-overlay={td}/overlay"   # td: private NOR
    child = subprocess.Popen([a.qemu, "-machine", machine,
                              "-display", "none", "-audio", "driver=none", "-monitor", "none", "-qmp", f"unix:{qmp},server=on,wait=off",
                              "-serial", f"file:{a.out}/serial.log"],
                             stdout=subprocess.DEVNULL, stderr=open(f"{a.out}/stderr", "w"))
    t0 = time.monotonic()
    while not os.path.exists(qmp):
        time.sleep(0.05)
    rig, syms = Rig(qmp), (None if v4 else Symbols())   # the symbol tables are 7B500's
    samples, stop = [], threading.Event()
    frame_path = f"{td}/f.ppm"

    def screen():
        rig.cmd("screendump", filename=frame_path)
        return read_ppm(frame_path)

    def sampler():
        while not stop.is_set():
            r = rig.cmd("human-monitor-command", **{"command-line": "info registers"})
            m, c = re.search(r"R15=([0-9a-f]{8})", r), re.search(r"PSR=([0-9a-f]{8})", r)
            if m and c:
                pc, mode = int(m.group(1), 16), int(c.group(1), 16) & 0x1f
                samples.append((time.monotonic() - t0, pc, mode != 0x10))
            time.sleep(0.05)

    def histogram(lo, hi, n=12):
        names = [syms.name(pc, k) if syms else ("kernel" if k else "user") for t, pc, k in samples if lo <= t < hi]
        tally = {}
        for x in names:
            tally[x] = tally.get(x, 0) + 1
        top = sorted(tally.items(), key=lambda kv: -kv[1])[:n]
        return {"samples": len(names), "top": [[k, round(v / max(1, len(names)), 3)] for k, v in top]}

    res = {}
    th = threading.Thread(target=sampler)
    th.start()
    try:
        # 1. lock screen lit, then knob tracking
        while True:
            t = time.monotonic() - t0
            if t > 240:
                raise SystemExit("no lit screen within 240 s")
            if lit(screen()):
                res["lock_lit_s"] = round(t, 2)
                time.sleep(a.first_probe_delay)
                break
            time.sleep(0.25)
        while True:
            t = time.monotonic() - t0
            if t - res["lock_lit_s"] > 120:
                res["knob_tracks_s"] = None
                break
            before = screen()
            if not lit(before):                    # blanked: wake it and keep probing
                rig.button("button-home")
                time.sleep(0.5)
                continue
            rig.ev(959, 477); rig.ev(down=True); time.sleep(0.05)
            for y in (437, 387, 337, 307):
                rig.ev(959, y); time.sleep(0.03)
            time.sleep(0.3)
            after = screen()
            for y in (337, 387, 437, 477):
                rig.ev(959, y); time.sleep(0.02)
            rig.ev(down=False)
            moved = np.abs(after[KNOB[1], KNOB[0]].astype(int) - before[KNOB[1], KNOB[0]].astype(int)).mean()
            res.setdefault("probes", []).append([round(t, 2), round(float(moved), 1)])
            from PIL import Image
            Image.fromarray(after).save(f"{a.out}/probe-{len(res['probes'])}.png")
            if moved > 4:
                res["knob_tracks_s"] = round(time.monotonic() - t0, 2)
                break
            time.sleep(PROBE)
        if res["knob_tracks_s"]:
            res["frozen_gap_s"] = round(res["knob_tracks_s"] - res["lock_lit_s"], 2)
            res["gap_pc"] = histogram(res["lock_lit_s"], res["knob_tracks_s"])

        # 2. unlock, Settings, touch-to-photon
        time.sleep(1)
        if not lit(screen()):
            rig.button("button-home"); time.sleep(1.5)
        rig.drag(959, 477, 959, 57)
        time.sleep(3)
        rig.tap(608, 382)                          # first-unlock tip
        time.sleep(1.5)
        rig.tap(*((445, 470) if v4 else (448, 649)))   # Settings (4.x's third row holds Game Center)
        time.sleep(8)
        from PIL import Image
        Image.fromarray(screen()).save(f"{a.out}/settings.png")
        quiet = time.monotonic() - t0
        time.sleep(3)
        res["idle_pc"] = histogram(quiet, time.monotonic() - t0)
        lat = []
        for k in range(a.samples):
            x, y = ROWS[k % len(ROWS)]
            region = (slice(x - 18, x + 18), slice(487, 747))
            base = screen()[region[1], region[0]].astype(int)
            rig.ev(x, y)
            td0 = time.monotonic()
            rig.ev(down=True)
            got = None
            while time.monotonic() - td0 < 3:
                t1 = time.monotonic()
                cur = screen()[region[1], region[0]].astype(int)
                if np.abs(cur - base).mean() > 4:
                    got = round((time.monotonic() - td0) * 1000)
                    break
                time.sleep(max(0.0, 1 / 60 - (time.monotonic() - t1)))
            rig.ev(down=False)
            lat.append(got)
            res.setdefault("tap_at_s", []).append(round(td0 - t0, 2))
            time.sleep(2)
        res["tap_to_highlight_ms"] = lat
        ok = [v for v in lat if v is not None]
        if ok:
            res["tap_to_highlight_median_ms"] = statistics.median(ok)
    finally:
        stop.set()
        th.join()
        try:
            rig.cmd("quit")
        except Exception:
            pass
        child.wait(timeout=20)
        subprocess.run(["rm", "-rf", td])
    json.dump(res, open(f"{a.out}/result.json", "w"), indent=1)
    json.dump([[round(t, 3), pc, k] for t, pc, k in samples], open(f"{a.out}/pc-samples.json", "w"))
    print(json.dumps(res, indent=1))


if __name__ == "__main__":
    main()
