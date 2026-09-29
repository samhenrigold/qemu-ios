#!/usr/bin/env python3
"""GL check that needs no app launch, so no activation: the it_gltest fixture job.

    tests/ipad1/gltest.py DEVICE [--out DIR] [--qemu Q]

DEVICE is a device dir made with `imgtools/ipad1_device.py create MANIFEST DEVICE --gl-test` (any
firmware; the job starts 12 s into every boot). Boots it on a throwaway overlay and private NOR copy,
waits for the fixture's first present, wakes the panel (Home), then:
  readback  the fixture's own glReadPixels of four probes says PASS (the host drew the scene)
  scene     two screendumps 3 s apart each hold magenta, cyan and yellow, the colours nothing in the
            iOS UI uses, in the 400x600 layer's proportions (1/4, 1/2, 1/4 of it) and no more than the
            layer's area: CoreAnimation composited the GL layer where it was placed
  fps       the fixture's own present rate (its "frame N at T" lines), and back-to-back screendumps
            while its blue band sweeps: distinct frames/s (bounded by screendump speed), and
            tests/ipad1/tearcheck.py --analyze scores those frames for tearing
Colours are classified against the frame's own maximum (the backlight scales pixels), as the iPod
GLES check does. Exit 0 if readback and scene pass; fps is reported, not judged.
"""
import argparse, hashlib, json, os, shutil, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "imgtools"))
import itqmp  # noqa: E402
from PIL import Image  # noqa: E402

LAYER = 400 * 600 / (1024 * 768)      # the fixture's layer, as a fraction of the panel
WANT = {"magenta": LAYER / 4, "cyan": LAYER / 2, "yellow": LAYER / 4}


def census(path):
    """{colour: fraction of the frame}."""
    im = Image.open(path).convert("RGB")
    px = list(im.get_flattened_data()) if hasattr(im, "get_flattened_data") else list(im.getdata())
    hi = max(max(p) for p in px) or 1
    lo, up = 0.3 * hi, 0.7 * hi
    n = {"magenta": 0, "cyan": 0, "yellow": 0}
    for r, g, b in px:
        if r >= up and b >= up and g <= lo:
            n["magenta"] += 1
        elif g >= up and b >= up and r <= lo:
            n["cyan"] += 1
        elif r >= up and g >= up and b <= lo:
            n["yellow"] += 1
    return {k: v / len(px) for k, v in n.items()}


def scene_problem(c):
    for k, want in WANT.items():
        if not 0.8 * want <= c[k] <= 1.05 * want:
            return "%s %.3f of the frame, want %.3f" % (k, c[k], want)
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("device")
    ap.add_argument("--out", default=None)
    ap.add_argument("--qemu", default=os.path.join(ROOT, "build/qemu-system-arm"))
    ap.add_argument("--burst", type=float, default=8.0, help="seconds of back-to-back screendumps")
    a = ap.parse_args()
    dev = os.path.abspath(a.device)
    lock = json.load(open(os.path.join(dev, "device.lock.json")))
    if not lock.get("gl_test"):
        raise SystemExit("%s was not made with --gl-test (no it_gltest job)" % dev)
    out = a.out or tempfile.mkdtemp(prefix="gltest.", dir=os.path.expanduser("~/Developer/qemu-ios-files/ipad1/repro"))
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out + "/frames")
    import ipad1_boot
    a.device = dev          # boot DEVICE's own iBoot, NOR and catalog keys, not ipad1_boot's default device
    machine = "ipad1,%s,nand=%s/nand,nand-overlay=%s/overlay,gles-debug=on" % (
        ipad1_boot.boot_options(a, out), dev, out)
    sock, serial = "/tmp/ipad1-gltest-%d.qmp" % os.getpid(), out + "/serial.log"
    itqmp.W, itqmp.H = 1024, 768
    q = None
    p = subprocess.Popen(["timeout", "180", a.qemu, "-machine", machine, "-display", "none", "-audio", "driver=none", "-monitor", "none",
                          "-serial", "file:" + serial, "-qmp", "unix:%s,server,nowait" % sock],
                         stdout=subprocess.DEVNULL, stderr=open(out + "/qemu.log", "w"), stdin=subprocess.DEVNULL)
    results, ok = {}, True
    try:
        while not os.path.exists(sock):
            if p.poll() is not None:
                raise SystemExit(open(out + "/qemu.log").read())
            time.sleep(0.2)
        q = itqmp.QMP(sock)
        t0 = time.time()
        text = ""
        while "it_gltest: presented" not in text and "-> FAIL" not in text:
            if p.poll() is not None or time.time() - t0 > 120:
                break
            time.sleep(0.5)
            text = open(serial, errors="replace").read()
        line = next((l for l in text.splitlines() if "it_gltest: readback" in l), "(no readback line)")
        results["readback"] = line.strip()
        ok &= line.endswith("PASS")
        itqmp.button(q, "home")          # wake the panel if it idled off
        time.sleep(2)
        for n in ("a", "b"):
            q.cmd("screendump", filename="%s/%s.ppm" % (out, n))
            time.sleep(0.5)
            Image.open("%s/%s.ppm" % (out, n)).save("%s/%s.png" % (out, n))
            c = census("%s/%s.png" % (out, n))
            why = scene_problem(c)
            results["scene " + n] = {"fractions": {k: round(v, 4) for k, v in c.items()}, "problem": why}
            ok &= why is None
            time.sleep(3)
        times, last, tn = [], None, time.time()
        while time.time() - tn < a.burst:
            q.cmd("screendump", filename=out + "/cur.ppm")
            h = hashlib.md5(open(out + "/cur.ppm", "rb").read()).hexdigest()
            if h != last:
                os.replace(out + "/cur.ppm", "%s/frames/f%05d.ppm" % (out, len(times)))
                times.append(time.time() - tn)
                last = h
        dt = time.time() - tn
        json.dump(times, open(out + "/frames/times.json", "w"))
        results["capture fps"] = round(len(times) / dt, 1)
        marks = [tuple(map(int, l.split()[2::2])) for l in open(serial, errors="replace")
                 if l.startswith("it_gltest: frame ")]
        if len(marks) >= 2:
            (f0, s0), (f1, s1) = marks[1], marks[-1]
            results["present fps (guest)"] = round((f1 - f0) / max(s1 - s0, 1), 1)
        tc = subprocess.run([sys.executable, os.path.join(HERE, "tearcheck.py"), "--analyze", out + "/frames"],
                            capture_output=True, text=True)
        results["tearcheck"] = (tc.stdout.strip().splitlines() or ["(no output)"])[-3:]
        # Everything the bridge refused since boot, host and shim sides (the fixture's scene is
        # magenta itself, so the counters are the signal here, not gles-debug's paint).
        results["rejects"] = itqmp.gles_rejects(q)
        ok &= not results["rejects"]
    finally:
        if q:
            try:
                q.cmd("quit")
            except Exception:
                pass
        try:
            p.wait(timeout=20)
        except subprocess.TimeoutExpired:
            p.kill()
        if os.path.exists(sock):
            os.unlink(sock)
    unimpl = sorted({l.strip() for f in (serial, out + "/qemu.log")
                     for l in open(f, errors="replace") if "unimplemented GL entry point" in l})
    results["unimplemented"] = unimpl
    print(json.dumps(results, indent=1))
    print("%s: %s" % ("PASS" if ok else "FAIL", out))
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
