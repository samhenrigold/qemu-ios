#!/usr/bin/env python3
"""Frames per second while CoreAnimation animates, for comparing software and GL CA.

    tests/ipad1/animfps.py STORE OUT

Boots STORE on an overlay in OUT (the store is never written), unlocks, then runs a fixed scenario
(search-page swipe and back, open+close Notes, open+close Calendar, twice) while a second thread
screendumps back to back through the same QMP session. Reports frames/s WHILE ANIMATING: frame changes
divided by the time inside runs of changes (gaps < 0.25 s), per-run counts, and totals. The distinct
frames land in OUT/frames with times.json, so `tearcheck.py --analyze OUT/frames` scores them
for tearing. Build the stores with `ipad1_rootfs.py build --gles [--ca-ogl]` (docs/ipad1/userland-gl-display.md).
"""
import hashlib, os, subprocess, sys, threading, time
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "imgtools"))
import itqmp
itqmp.W, itqmp.H = 1024, 768
store, out = sys.argv[1:3]
os.makedirs(out, exist_ok=True)
sock = "/tmp/a4g-anim-%d.qmp" % os.getpid()
serial = out + "/serial.log"
qemu = subprocess.Popen(["timeout", "500", os.path.join(ROOT, "build/qemu-system-arm"),
    "-machine", "ipad1,kboot=%s,nand=%s,nand-overlay=%s/overlay" % (os.path.expanduser("~/Developer/qemu-ios-files/ipad1/7B500/k48-kboot.bin"), store, out),
    "-display", "none", "-monitor", "none", "-serial", "file:" + serial, "-qmp", "unix:%s,server,nowait" % sock],
    stdout=subprocess.DEVNULL, stderr=open(out + "/qemu.log", "w"))
try:
    while not os.path.exists(sock): time.sleep(0.2)
    q = itqmp.QMP(sock)
    t0 = time.time()
    while "_lcdEnable: enable: 0" not in open(serial, errors="replace").read():
        if time.time() - t0 > 300: sys.exit("no lock screen")
        time.sleep(2)
    itqmp.button(q, "home"); time.sleep(2)
    itqmp.swipe(q, 64, 290, 64, 720, steps=20, dt=0.03); time.sleep(4)
    itqmp.tap(q, 415, 385); time.sleep(3)          # first-boot "Edit Home Screen" dialog
    lock, raw = threading.Lock(), q.cmd             # one QMP session, shared by input and the dumper
    def locked(*a, **k):
        with lock:
            return raw(*a, **k)
    q.cmd = locked
    q2 = q
    samples, stop = [], [False]
    os.makedirs(out + "/frames", exist_ok=True)
    def dumper():
        import shutil, json
        while not stop[0]:
            q2.cmd("screendump", filename=out + "/d.ppm")
            h = hashlib.md5(open(out + "/d.ppm", "rb").read()).hexdigest()
            if not samples or h != samples[-1][1]:
                shutil.copy(out + "/d.ppm", out + "/frames/f%05d.ppm" % len(samples))
            samples.append((time.time(), h))
        json.dump([t for t, _ in samples], open(out + "/frames/times.json", "w"))
    th = threading.Thread(target=dumper); th.start()
    for _ in range(2):
        itqmp.swipe(q, 500, 700, 500, 100, steps=15, dt=0.02); time.sleep(2.5)   # to the search page and back
        itqmp.swipe(q, 500, 100, 500, 700, steps=15, dt=0.02); time.sleep(2.5)
        itqmp.tap(q, 895, 470); time.sleep(4)                                     # Notes
        itqmp.button(q, "home"); time.sleep(3)
        itqmp.tap(q, 895, 118); time.sleep(4)                                     # Calendar
        itqmp.button(q, "home"); time.sleep(3)
    stop[0] = True; th.join()
    ch = [samples[i][0] for i in range(1, len(samples)) if samples[i][1] != samples[i - 1][1]]
    active, runs, start, last, n, per = 0.0, 0, None, None, 0, []
    for t in ch:
        if last is None or t - last > 0.25:
            if start is not None:
                active += last - start; per.append((n, last - start))
            start, runs, n = t, runs + 1, 0
        last = t; n += 1
    if start is not None:
        active += last - start; per.append((n, last - start))
    print("runs (changes/seconds):", " ".join("%d/%.2f" % r for r in per if r[0] > 2))
    span = samples[-1][0] - samples[0][0]
    print("%s: %d dumps in %.1fs (%.0f/s), %d frame changes in %d runs, %.1fs animating -> %.1f frames/s while animating"
          % (os.path.basename(out), len(samples), span, len(samples) / span, len(ch), runs, active,
             (len(ch) - runs) / active if active else 0))
    q.cmd("quit")
finally:
    try: qemu.wait(timeout=20)
    except Exception: qemu.kill()
    if os.path.exists(sock): os.unlink(sock)
