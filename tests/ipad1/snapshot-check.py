#!/usr/bin/env python3
"""Live snapshot round trip for the ipad1 machine: save mid-use, quit, resume, and keep using it.

    tests/ipad1/snapshot-check.py [--out DIR] [--device DIR] [--nand STORE] [--kboot K] [--qemu Q]

Boot A (golden overlay, usbmuxd bridge, USB keyboard, Wi-Fi): unlock, Safari fetches page 1 from a host
HTTP server. Wi-Fi's slirp carries the app's web proxy forward, 10.0.2.100:3128 (here to the test's
server, which also answers the page's own 10.0.2.50:80). The bridge's USB Ethernet is up too, on
usbmuxd's own 10.0.2.0/24, so Wi-Fi is on 10.0.2.0/25: the longer prefix routes .50 and .100 over Wi-Fi
whichever service is primary, and only Wi-Fi's slirp answers them, so a hit crossed Wi-Fi whether the
image's PAC sends it DIRECT or through the proxy. Stop, migrate to DIR/state, copy the overlay beside it (the app's pairing:
RAM in the stream, flash in the overlay, captured with the vCPU stopped), quit.

Boot B: a new usbmuxd, the copied overlay, -incoming DIR/state, cont. Then:
  screen   Safari still shows page 1 (the frame matches A's last one)
  touch    tapping the address field and typing page 2 on the USB keyboard fetches it (touch, keyboard, Wi-Fi)
  usbmux   the new bridge reaches the restored guest: ideviceinfo answers ProductVersion

Then audio: in B, Hold (the lock sound starts) and save again at once, mid-sound. Boot C resumes that
with -audio driver=wav, and unlock, lock, unlock must each come out and correlate with the stock sound
files (audio-check.py's judge), so the paced CDMA/I2S path survives a save taken while it runs.
Exit 0 if all pass.
"""
import argparse
import http.server
import importlib.util
import os
import random
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("ipad1_regress", os.path.join(HERE, "regress.py"))
rg = importlib.util.module_from_spec(spec)
spec.loader.exec_module(rg)
itqmp, log = rg.itqmp, rg.log
spec = importlib.util.spec_from_file_location("audio_check", os.path.join(HERE, "audio-check.py"))
ac = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ac)


def save(boot, snap):
    """Stop, stream RAM to snap/state, copy the overlay beside it (vCPU stopped, so the two agree)."""
    os.makedirs(snap, exist_ok=True)
    boot.qmp.cmd("stop")
    shot = boot.shot("before")
    boot.qmp.cmd("migrate", uri="file:" + os.path.join(snap, "state"))
    t0 = time.time()
    while boot.qmp.cmd("query-migrate").get("status") not in ("completed", "failed") and time.time() - t0 < 120:
        time.sleep(0.5)
    st = boot.qmp.cmd("query-migrate").get("status")
    if st != "completed":
        sys.exit("save: migration %s" % st)
    subprocess.run(["cp", "-cR", boot.overlay, os.path.join(snap, "overlay")], check=True)   # + its private nor.bin
    log("saved %s: %d MiB state + overlay" % (snap, os.path.getsize(os.path.join(snap, "state")) >> 20))
    return shot


def resume(cfg, tag, snap, extra, wav=None):
    b = rg.Boot(cfg, tag, keyboard=True, overlay=os.path.join(snap, "overlay"), wav=wav,
                extra=extra + ["-incoming", "file:" + os.path.join(snap, "state")])
    b.start()
    t0 = time.time()
    while b.qmp.cmd("query-status")["status"] == "inmigrate" and time.time() - t0 < 120:
        time.sleep(0.5)
    b.qmp.cmd("cont")
    return b


def newest(d):
    """Latest mtime of an overlay directory and its files, as the app judges it."""
    return max([os.stat(d).st_mtime] + [os.stat(os.path.join(d, f)).st_mtime for f in os.listdir(d)])


def frame_diff(a, b):
    """Fraction of sampled sub-pixels that differ by more than 24."""
    _, _, pa = itqmp.read_ppm(a)
    _, _, pb = itqmp.read_ppm(b)
    idx = range(0, min(len(pa), len(pb)), 13)
    return sum(1 for i in idx if abs(pa[i] - pb[i]) > 24) / len(idx)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--nand", help="override the selected device NAND")
    rg.ipad1_boot.add_arguments(ap)
    ap.add_argument("--qemu", default=os.path.join(rg.ROOT, "build/qemu-system-arm"))
    ap.add_argument("--usbmuxd", default=rg.USBMUXD)
    ap.add_argument("--boot-timeout", type=int, default=900)
    ap.add_argument("--out", default=None)
    ap.add_argument("--sound-reference-root", help="explicit matching extracted rootfs when the guest has no agent")
    ap.add_argument("--guest-package", help="current guest-agent offer for the selected device")
    cfg = ap.parse_args()
    rg.device_args(cfg)
    rg.ipod.START = time.time()
    cfg.out = cfg.out or tempfile.mkdtemp(prefix="ipad1snap-")
    snap, snap2 = os.path.join(cfg.out, "snap"), os.path.join(cfg.out, "snap2")

    hits = []

    class H(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            hits.append(urllib.parse.urlsplit(self.path).path)   # absolute URL when it came via the proxy
            body = ("<html><body style='font-size:48px'><h1>%s</h1></body></html>" % self.path).encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, *a):
            pass

    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), H)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    port = srv.server_address[1]
    tok = random.randrange(1 << 30)
    page1, page2 = "/snap1-%d" % tok, "/snap2-%d" % tok

    def fetched(path, timeout=60):
        t0 = time.time()
        while path not in hits and time.time() - t0 < timeout:
            time.sleep(1)
        return path in hits

    results = {}
    fwd = "-cmd:nc 127.0.0.1 %d" % port
    wifi = ["-netdev", "user,id=wifi0,net=10.0.2.0/25,dhcpstart=10.0.2.80,guestfwd=tcp:10.0.2.100:3128%s,guestfwd=tcp:10.0.2.50:80%s" % (fwd, fwd)]
    a = rg.Boot(cfg, "A", keyboard=True, extra=wifi)
    try:
        a.start()
        ok, detail = a.wait_lock_screen()
        if not ok or not a.wait_mux():
            sys.exit("boot A: %s" % (detail if not ok else "usbmux never attached"))
        a.drag(rg.UNLOCK_FROM, rg.UNLOCK_TO)
        expect = ac.capture_references(a.qmp, os.path.join(cfg.out, "references"),
                                       cfg.product_version, cfg.sound_reference_root)
        a.tap(rg.SAFARI_ICON)
        time.sleep(8)
        a.tap(rg.SAFARI_ADDRESS)
        time.sleep(5)
        a.type("10.0.2.50" + page1)
        a.qmp.cmd("send-key", keys=[{"type": "qcode", "data": "ret"}])
        if not fetched(page1):
            sys.exit("boot A: Safari never fetched page 1")
        time.sleep(6)
        before = save(a, snap)
    finally:
        a.stop()
    # The app's pairing rule (DeviceStateStorage.overlayIsNewer): a snapshot is only restorable if the
    # overlay has not moved since. Check both directions: after a clean save and quit it has not...
    state_m = os.stat(os.path.join(snap, "state")).st_mtime
    results["pairing"] = [newest(a.overlay) <= state_m, "overlay not newer than the state after save + quit"]

    b = None
    try:
        b = resume(cfg, "B", snap, wifi)
        time.sleep(3)
        d = frame_diff(before, b.shot("after"))
        results["screen"] = (d < 0.05, "%.1f%% of the frame differs from the saved one" % (d * 100))

        b.tap(rg.SAFARI_ADDRESS)
        time.sleep(4)
        b.type("10.0.2.50" + page2)
        b.qmp.cmd("send-key", keys=[{"type": "qcode", "data": "ret"}])
        results["touch"] = (fetched(page2), "tap + USB keyboard + Wi-Fi fetch of page 2 after resume")
        time.sleep(4)
        b.shot("page2")

        mux = b.wait_mux(timeout=120)
        info = b.run(["ideviceinfo", "-k", "ProductVersion"], timeout=60) if mux else None
        pv = info.stdout.strip() if info else ""
        results["usbmux"] = (pv == cfg.product_version, "new usbmuxd: %s" % (("ProductVersion %s" % pv) if mux else "never attached"))

        # Mid-sound save: Hold starts the lock sound; stop the machine while it plays.
        b.press("hold", hold=0.2)
        time.sleep(0.1)
        save(b, snap2)
        results["panic"] = ("panic(" not in open(b.serial, errors="replace").read(), "no kernel panic in B")
        # ...and once a resumed guest writes flash, the overlay is newer, so a stale snapshot is refused.
        adv = newest(os.path.join(snap, "overlay")) > state_m
        results["pairing"] = (results["pairing"][0] and adv,
                              results["pairing"][1] + "; newer after the resumed guest wrote" if adv
                              else "overlay written after resume but not newer than the state")
    finally:
        if b:
            b.stop()
    wav = os.path.join(cfg.out, "C.wav")
    c = None
    try:
        c = resume(cfg, "C", snap2, wifi, wav=wav)   # Boot's own -audio (it defaults to driver=none)
        time.sleep(5)
        # audio-check's play_sounds, but through the button-* properties: the USB keyboard in the
        # saved machine owns QMP keys, so key-chord buttons would type instead.
        for step in ("unlock", "lock", "unlock"):
            if step == "lock":
                c.press("hold")
                time.sleep(4)
                continue
            c.press("home")
            time.sleep(2)
            c.drag(rg.UNLOCK_FROM, rg.UNLOCK_TO)
            time.sleep(6)
        time.sleep(3)
        results["panic"] = (results["panic"][0] and "panic(" not in open(c.serial, errors="replace").read(),
                            "no kernel panic in B or C")
    finally:
        if c:
            c.stop()
        srv.shutdown()
    cap, rate = ac.load(wav)
    spans = ac.events(cap, rate)[-3:]          # a leftover tail of the saved lock sound may come first
    with tempfile.TemporaryDirectory() as td:
        corrs = [ac.corr(cap[max(0, s0 - rate // 10):s1 + rate // 10], ac.reference(path, td))
                 for (s0, s1), (_, path) in zip(spans, expect[1:])]
    results["audio"] = (len(corrs) == 3 and min(corrs) >= ac.MIN_CORR,
                        "unlock/lock/unlock after a mid-sound resume, corr %s" % " ".join("%.2f" % x for x in corrs))

    for k, (ok, detail) in results.items():
        print("%s  %-7s %s" % ("PASS" if ok else "FAIL", k, detail))
    print("artifacts in %s" % cfg.out)
    sys.exit(0 if all(ok for ok, _ in results.values()) else 1)


if __name__ == "__main__":
    main()
