#!/usr/bin/env python3
"""Animation-jank audit in guest-virtual time (docs: LightTouchMac docs/perf-jank.md).

    tests/ipad1/jank.py [--device DIR] [--qemu Q] [--out DIR]   # measure the three canonical animations
    tests/ipad1/jank.py --gate                                  # ... and check them against jank-baselines.json
    tests/ipad1/jank.py --rescore OUT/jank.json [--gate]        # re-score a saved run, no boot
    IT_JANK_STALL_EVERY=4 tests/ipad1/jank.py --gate            # proof: hold one swap in 4 -> the gate FAILs

Boots the ipad1 machine (regress.py's Boot; 3.2.2 golden, iop-core=off) under -icount shift=0,sleep=off, so
QEMU_CLOCK_VIRTUAL counts guest instructions instead of following the host clock. Then, with the machine
paused, it drives each gesture on exact vsync steps (the display's stop-after-vsyncs) and reads the display's
frame-timeline ring: one entry per panel vsync, stamped in virtual ns, new or held (hw/arm/frame-timeline.h).
Every number is computed from those stamps, so the verdict is the same at any host load. The old fps numbers
were sampled on the host clock and moved with the machine's load (the 2026-09-29 test audit, gap #7).

Per animation, over a fixed virtual-time window split into runs at >100 ms rests: p50/p95/p99 frame interval,
hitches (an interval of 2+ vsyncs, >=33 ms: a missed refresh), dropped (held vsyncs while animating), longest
interval, runs, frames.
Blind spot, by construction: host-side emulator cost (a device handler, the GL bridge) takes no virtual time.
"""
import argparse, importlib.util, json, os, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
_spec = importlib.util.spec_from_file_location("ipad1_regress", os.path.join(ROOT, "tests/ipad1/regress.py"))
R = importlib.util.module_from_spec(_spec); _spec.loader.exec_module(R)

# The guest's intended cadence: both panels raise a 60 Hz frame interrupt (VBL_PERIOD_NS in
# s5l8930_display.c, LCD_VSYNC_PERIOD_NS in ipod_touch_lcd.h), which is what CADisplayLink paces to.
BUDGET_MS = 1000.0 / 60.0          # 16.67 ms: one vsync
HITCH_MS = 1.5 * BUDGET_MS         # an interval of 2+ vsyncs (>=33.3 ms): a frame that missed a refresh
IDLE_MS = 100.0                    # a gap over 6 vsyncs is the screen at rest, not a late frame
BASELINES = os.path.join(HERE, "jank-baselines.json")


def find_prop(qmp, name, path="/machine", depth=0):
    """QOM path of the object carrying property NAME (the display device), walked like perf_ipad's find_swaps."""
    for p in qmp.cmd("qom-list", path=path):
        if p["name"] == name:
            return path
    if depth > 3:
        return None
    for p in qmp.cmd("qom-list", path=path):
        if p["type"].startswith("child<"):
            f = find_prop(qmp, name, path.rstrip("/") + "/" + p["name"], depth + 1)
            if f:
                return f
    return None


def read_timeline(qmp, path):
    """[(seq, virt_ns, newframe, key)] oldest-to-newest from the frame-timeline ring."""
    text = qmp.cmd("qom-get", path=path, property="frame-timeline")
    out = []
    for line in text.splitlines():
        if line:
            s, v, nf, k = line.split()
            out.append((int(s), int(v), int(nf), int(k)))
    return out


def pct(xs, p):
    if not xs:
        return 0.0
    xs = sorted(xs)
    k = (len(xs) - 1) * p / 100.0
    f = int(k)
    c = min(f + 1, len(xs) - 1)
    return xs[f] + (xs[c] - xs[f]) * (k - f)


def analyze(entries, seq0, window_ms):
    """Jank stats for the WINDOW_MS of virtual time after frame seq0 (the gesture's first input).

    New frames are split into runs at gaps over IDLE_MS: inside a run the screen is animating, so an interval
    over the budget is a hitch and a held vsync is a dropped frame; a longer gap is the screen at rest (a
    finger holding still, an app between two animations). A stall long enough to split an animation shows as
    an extra run, which the gate also counts."""
    after = [(s, v, nf) for (s, v, nf, k) in entries if s > seq0]
    if not after:
        return {"frames": 0, "verdict": "no timeline"}
    t0 = after[0][1]
    win = [(s, v, nf) for (s, v, nf) in after if v - t0 <= window_ms * 1e6]
    new = [v for (s, v, nf) in win if nf]
    if len(new) < 2:
        return {"frames": len(new), "verdict": "no animation captured"}
    runs, cur = [], [new[0]]
    for v in new[1:]:
        if (v - cur[-1]) / 1e6 > IDLE_MS:
            runs.append(cur); cur = [v]
        else:
            cur.append(v)
    runs.append(cur)
    intervals = [(q - p) / 1e6 for r in runs for p, q in zip(r, r[1:])]        # ms, virtual time, in-run
    dropped = sum(1 for (s, v, nf) in win if not nf and any(r[0] < v < r[-1] for r in runs))
    hitches = [i for i in intervals if i > HITCH_MS]
    r = {
        "frames": len(new),
        "runs": len(runs),
        "first_frame_ms": round((new[0] - t0) / 1e6, 1),
        "p50_ms": round(pct(intervals, 50), 1),
        "p95_ms": round(pct(intervals, 95), 1),
        "p99_ms": round(pct(intervals, 99), 1),
        "hitches_gt_33ms": len(hitches),
        "longest_ms": round(max(intervals), 1) if intervals else 0.0,
        "dropped": dropped,
        "load1": round(os.getloadavg()[0], 1),
    }
    r["verdict"] = ("%d frames in %d run%s, p99 %.1f ms, %d hitches (>=33 ms), longest %.1f ms, %d dropped"
                    % (r["frames"], r["runs"], "" if r["runs"] == 1 else "s", r["p99_ms"], r["hitches_gt_33ms"],
                       r["longest_ms"], r["dropped"]))
    return r


def run_vsyncs(b, path, n, ev=None):
    """Run the (paused) machine for exactly N panel vsyncs of virtual time, then it pauses itself
    (the display's stop-after-vsyncs). Wall time taken is whatever the host needs; virtual time is fixed.
    EV is queued while paused and delivered by the display at the window's first vsync (vsync-input), so
    its guest time is exact; a button goes through the machine's button-* property the same way."""
    if ev:
        inject(b, path, ev)
    b.qmp.cmd("qom-set", path=path, property="stop-after-vsyncs", value=n)
    b.qmp.cmd("cont")
    while b.qmp.cmd("query-status")["running"]:
        time.sleep(0.02)


# A gesture is a script of (input, vsyncs to run after it), injected on exact vsyncs -- the way a 60 Hz
# digitizer reports -- so its timing is the same in guest time under any host load. Panel coordinates of the
# upright portrait UI (see regress.py); Notes sits at (128, 297) on 3.2.2's home screen.
def _drag(a, z, steps=15, per=2):
    s = [(("move",) + a, 1), (("down",), 9)]
    s += [(("move", a[0] + (z[0] - a[0]) * i / steps, a[1] + (z[1] - a[1]) * i / steps), per) for i in range(1, steps + 1)]
    return s + [(("wait",), 18), (("up",), 1)]           # rest before release: a moving release reads as a flick back

# (name, script, measured window in virtual ms from the first input; None = not measured). The windows cover
# the animation and stop before what follows it: app-launch's ends before Notes' caret starts blinking.
GESTURES = [("home-swipe", _drag((523, 167), (523, 617)), 1500),         # drag to the Spotlight page + settle
            ("home-back",  [(("button", "home", True), 18), (("button", "home", False), 1)], None),   # reset
            ("app-launch", [(("move", 128, 297), 1), (("down",), 7), (("up",), 1)], 500),   # open Notes: launch zoom
            ("app-close",  [(("button", "home", True), 18), (("button", "home", False), 1)], 1000)]  # close zoom
QUIET_VSYNCS, MAX_VSYNCS = 60, 900                     # 1 s without a new frame ends it; at most 15 s


def inject(b, path, ev):
    """Queue one input on the paused machine (panel pixels scaled as regress.py's Boot.ev does)."""
    if ev[0] == "move":
        b.qmp.cmd("qom-set", path=path, property="vsync-input",
                  value="abs %d %d" % (int(ev[1] * 32767 / 1024), int(ev[2] * 32767 / 768)))
    elif ev[0] in ("down", "up"):
        b.qmp.cmd("qom-set", path=path, property="vsync-input", value="btn %d" % (ev[0] == "down"))
    elif ev[0] == "button":
        b.qmp.cmd("qom-set", path="/machine", property="button-" + ev[1], value=ev[2])


def drive(b, path, script, window_ms):
    """Run a gesture script on exact vsyncs, then keep stepping until 1 s of virtual quiet; return stats.
    The machine is paused between steps, so reading the timeline cannot perturb the frames it measures."""
    tl = read_timeline(b.qmp, path)
    seq0 = tl[-1][0] if tl else 0
    for ev, n in script:
        run_vsyncs(b, path, n, ev)
    ran = 0
    while ran < MAX_VSYNCS:
        run_vsyncs(b, path, QUIET_VSYNCS)
        ran += QUIET_VSYNCS
        tl = read_timeline(b.qmp, path)
        after = [e for e in tl if e[0] > seq0]
        if any(e[2] for e in after) and not any(e[2] for e in after[-QUIET_VSYNCS:]):
            break
    r = analyze(tl, seq0, window_ms or 1000)
    r["timeline"] = [e for e in tl if e[0] > seq0]           # raw (seq, virt_ns, newframe, key), for the record
    return r


def measure(cfg):
    """Boot, unlock, drive every canonical animation on one boot. Returns {name: stats}."""
    # No USB host and no USB keyboard: the jank harness only drives the emulated touchscreen over QMP and
    # reads the display property, so it needs neither usbmuxd nor the keyboard's "not supported" alert.
    # -icount: QEMU_CLOCK_VIRTUAL then counts retired guest instructions (shift=0: 1 ns each, a 1 GHz A4)
    # instead of following the host clock, which is what makes the timeline load-immune. Without it,
    # virtual time runs with host time while the VM runs, so a starved guest misses vsyncs (--no-icount
    # is the control that shows it). sleep=off: an idle guest jumps straight to its next timer, so idle time is
    # deterministic too, and frame intervals come out as exact multiples of the vsync period.
    extra = [] if cfg.no_icount else ["-icount", "shift=0,sleep=off"]
    # The default IOP second core (Apple's IOP firmware) never reaches the lock screen under icount; the HLE does.
    os.environ["IPAD1_MACHINE_EXTRA"] = ",".join(x for x in (os.environ.get("IPAD1_MACHINE_EXTRA"), "iop-core=off") if x)
    b = R.Boot(cfg, "jank", usb=False, keyboard=False, extra=extra)
    b.start()
    results = {}
    try:
        ok, detail = b.wait_lock_screen(timeout=900)
        if not ok:
            raise SystemExit("no lock screen: " + detail)
        if b.lit("pre-unlock") < R.LIT_MIN_FRACTION:
            b.press("home"); time.sleep(2)
        b.drag(R.UNLOCK_FROM, R.UNLOCK_TO)
        time.sleep(8)
        b.tap((608, 382)); time.sleep(3)                           # dismiss the first-unlock tip / alert, if any
        path = find_prop(b.qmp, "frame-timeline")
        if not path:
            raise SystemExit("no frame-timeline property (emulator not rebuilt with the instrumentation?)")
        R.log("jank: frame-timeline on %s" % path)
        b.qmp.cmd("stop")                                          # from here on the machine only runs in steps
        for name, script, window_ms in GESTURES:
            r = drive(b, path, script, window_ms)
            if window_ms:
                results[name] = r
                R.log("jank %-11s %s" % (name, r["verdict"]))
    finally:
        os.makedirs(cfg.out, exist_ok=True)
        json.dump(results, open(os.path.join(cfg.out, "jank.json"), "w"), indent=1)
        b.stop()
    return results


def gate(results):
    """Threshold every canonical animation against jank-baselines.json. Prints PASS/FAIL lines; returns ok."""
    base = json.load(open(BASELINES))
    ok = True
    for name in (n for n in base if not n.startswith("_")):
        r = results.get(name, {})
        b = base[name]
        fails = []
        if r.get("frames", 0) < b["min_frames"]:
            fails.append("frames %s < %d" % (r.get("frames"), b["min_frames"]))
        if r.get("p99_ms", 1e9) > b["max_p99_ms"]:
            fails.append("p99 %.1f > %d ms" % (r.get("p99_ms", -1), b["max_p99_ms"]))
        if r.get("hitches_gt_33ms", 999) > b["max_hitches"]:
            fails.append("hitches %s > %d" % (r.get("hitches_gt_33ms"), b["max_hitches"]))
        if r.get("longest_ms", 1e9) > b["max_longest_ms"]:
            fails.append("longest %.1f > %d ms" % (r.get("longest_ms", -1), b["max_longest_ms"]))
        if r.get("runs", 999) > b["max_runs"]:
            fails.append("runs %s > %d" % (r.get("runs"), b["max_runs"]))
        if r.get("dropped", 999) > b["max_dropped"]:
            fails.append("dropped %s > %d" % (r.get("dropped"), b["max_dropped"]))
        state = "FAIL" if fails else "PASS"
        ok = ok and not fails
        print("%s jank %-11s %s%s" % (state, name, r.get("verdict", "no data"),
                                      ("  [" + "; ".join(fails) + "]") if fails else ""))
    return ok


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--nand")
    R.ipad1_boot.add_arguments(ap)
    ap.add_argument("--qemu", default=os.path.join(ROOT, "build/qemu-system-arm"))
    ap.add_argument("--usbmuxd", default=R.USBMUXD)
    ap.add_argument("--boot-timeout", type=int, default=1500)
    ap.add_argument("--out", default=None)
    ap.add_argument("--rescore", metavar="JANK_JSON", help="re-score a run's saved timelines, no boot")
    ap.add_argument("--no-icount", action="store_true", help="control: host-clocked virtual time (load-dependent)")
    ap.add_argument("--gate", action="store_true", help="check against jank-baselines.json, exit non-zero on regression")
    a = ap.parse_args()
    if a.rescore:
        saved = json.load(open(a.rescore))
        windows = {n: w for n, _, w in GESTURES if w}
        results = {n: analyze([tuple(e) for e in r["timeline"]], -1, windows[n]) for n, r in saved.items()}
        for name, r in results.items():
            print("%-11s %s" % (name, r["verdict"]))
        sys.exit(0 if not a.gate or gate(results) else 1)
    R.device_args(a)
    a.out = a.out or __import__("tempfile").mkdtemp(prefix="jank-")
    R.ipod.START = time.time()
    results = measure(a)
    print("=" * 62)
    for name, r in results.items():
        print("%-11s %s" % (name, r.get("verdict", "no data")))
    print("results: %s" % os.path.join(a.out, "jank.json"))
    if a.gate:
        print("-" * 62)
        sys.exit(0 if gate(results) else 1)


if __name__ == "__main__":
    main()
