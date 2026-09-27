#!/usr/bin/env python3
"""Regression gate for the ipad1 machine (iPad 1, iOS 3.2.2), the iPod suite's counterpart.

    tests/ipad1/regress.py                     # default tier
    tests/ipad1/regress.py --checks net,afc    # explicit selection

Every check boots its own copy-on-write overlay of golden-pristine (the base is never written) with
usbmuxd-qemu's ipad1 build as the USB host, up to three QEMUs at once host-wide (pgrep -x).

  boot     lock screen on the panel: lit and not a solid fill, held HOME_CONFIRM_S
  usbmux   ideviceinfo over the bridge answers ProductVersion 3.2.2, DeviceClass iPad
  afc      push and pull files at sizes that are not multiples of 512, SHA-256 identical
  persist  a file pushed over AFC survives a reboot on the same overlay (see check_persist)
  net      USB Ethernet: Safari (typed on the emulated USB keyboard) fetches a page from a host HTTP
           server at 10.0.2.2, which needs the en1 DHCP lease from usbmuxd's slirp and the link patch
  audio, appinstall, applaunch, gles
           not yet: audio waits for a4-periph's WAV check; stock installd rejects every app not
           validly signed for the device (ApplicationVerificationFailed), so the app checks wait for
           a legitimately installable app or apps baked into the image

Exits non-zero if any selected check FAILs.
"""
import argparse
import concurrent.futures
import http.server
import importlib.util
import os
import random
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
spec = importlib.util.spec_from_file_location("ipod_regress", os.path.join(ROOT, "tests/ipod/regress.py"))
ipod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ipod)
itqmp = ipod.itqmp
Result, Procs, free_port, sha256_file, log = ipod.Result, ipod.Procs, ipod.free_port, ipod.sha256_file, ipod.log

FILES = os.path.expanduser("~/Developer/qemu-ios-files/ipad1")
USBMUXD = os.path.expanduser("~/Developer/usbmuxd-qemu-ipad1-net/src/usbmuxd")
DEFAULT_CHECKS = ["boot", "usbmux", "afc", "persist", "net"]
PENDING = {"audio": "waiting for a4-periph's WAV-correlation check",
           "appinstall": "stock installd rejects apps not validly signed for this device",
           "applaunch": "needs appinstall", "gles": "needs appinstall (GLTest is ldid-signed)"}
MAX_QEMUS = 3
# Scanout is 1024x768 with the portrait UI turned on it. The boot logo is a small Apple on black (a few %
# lit); the lock screen is a full wallpaper (~99% lit, unlike the iPod's dark panel). A stalled panel's
# solid fill is also fully lit, so the frame must also be a picture: many distinct colours.
LIT_MIN_FRACTION, MIN_COLOURS, HOME_CONFIRM_S = 0.5, 64, 2
# Panel coordinates (the UI is portrait, rotated onto the landscape scanout).
UNLOCK_FROM, UNLOCK_TO = (64, 290), (64, 720)
USB_ALERT_DISMISS = (475, 385)   # stock "The attached USB device is not supported." (the USB keyboard)
SAFARI_ICON, SAFARI_ADDRESS = (64, 117), (968, 330)

launch_lock = threading.Lock()


class Boot:
    """One QEMU on an overlay of the golden store, plus its usbmuxd."""
    n = 0

    def __init__(self, cfg, tag, overlay=None, keyboard=False):
        Boot.n += 1
        self.cfg, self.tag, self.keyboard = cfg, tag, keyboard
        self.dir = os.path.join(cfg.out, tag)
        os.makedirs(self.dir, exist_ok=True)
        self.overlay = overlay or os.path.join(self.dir, "overlay")
        self.procs = Procs()
        self.sock = "/tmp/ipad1rg-%d-%d.qmp" % (os.getpid(), Boot.n)   # sun_path < 104
        self.serial = os.path.join(self.dir, "serial.log")
        self.muxlog = os.path.join(self.dir, "usbmuxd.log")
        self.qmp = self.qemu = None

    def start(self):
        cfg = self.cfg
        with launch_lock:       # the host-wide QEMU budget, checked and claimed atomically for our threads
            while int(subprocess.run("pgrep -x qemu-system-arm | wc -l", shell=True, capture_output=True,
                                     text=True).stdout) >= MAX_QEMUS:
                time.sleep(15)
            self.usb_port = free_port(21300, 21399)
            self.mux_port = free_port(27400, 27499)
            env = dict(os.environ, USBMUXD_QEMU_ADDR="127.0.0.1:%d" % self.usb_port, USBMUXD_QEMU_DELAY="0")
            os.makedirs(os.path.join(self.dir, "conf"), exist_ok=True)
            self.procs.spawn([cfg.usbmuxd, "-f", "-v", "-v", "-S", "127.0.0.1:%d" % self.mux_port,
                              "-P", "NONE", "-C", os.path.join(self.dir, "conf")], self.muxlog, env=env)
            machine = "ipad1,kboot=%s,nand=%s,nand-overlay=%s,usb-tcp-addr=127.0.0.1:%d" % (
                cfg.kboot, cfg.nand, self.overlay, self.usb_port)
            argv = ["timeout", str(cfg.boot_timeout), cfg.qemu, "-machine", machine, "-display", "none",
                    "-monitor", "none", "-serial", "file:" + self.serial, "-qmp", "unix:%s,server,nowait" % self.sock]
            # A USB keyboard takes QMP keys ahead of the machine's button chords, so only boots that type get one.
            argv += ["-device", "usb-kbd,bus=usb-bus.0"] if self.keyboard else []
            self.qemu = self.procs.spawn(argv, os.path.join(self.dir, "qemu.log"))
            time.sleep(2)
        self.qmp = itqmp.QMP(self.sock, timeout=60)
        log("%s: qemu pid %d, usb %d, mux %d" % (self.tag, self.qemu.pid, self.usb_port, self.mux_port))

    def env(self):
        return dict(os.environ, USBMUXD_SOCKET_ADDRESS="127.0.0.1:%d" % self.mux_port)

    def run(self, argv, timeout=120):
        return subprocess.run(argv, env=self.env(), capture_output=True, text=True, timeout=timeout)

    def shot(self, name):
        ppm = os.path.join(self.dir, name + ".ppm")
        self.qmp.cmd("human-monitor-command", **{"command-line": "screendump " + ppm})
        time.sleep(0.5)
        return ppm

    def picture(self, name="screen"):
        """(is a lit picture, detail)."""
        try:
            w, h, pix = itqmp.read_ppm(self.shot(name))
        except Exception:
            return False, "no screendump"
        lit = sum(1 for v in pix[::7] if v > ipod.LIT_THRESHOLD) / len(pix[::7])
        colours = len({bytes(pix[i:i + 3]) for i in range(0, len(pix) - 2, 3 * 97)})
        return lit >= LIT_MIN_FRACTION and colours >= MIN_COLOURS, "lit %.0f%%, %d colours" % (lit * 100, colours)

    def wait_lock_screen(self, timeout=300):
        """(ok, detail): lit, not solid, and still so HOME_CONFIRM_S later.

        The lock screen turns the panel off about 8 s after it appears, so the confirmation is short and
        callers act right after. Boots without the USB keyboard press Home to wake a panel that went dark
        before it was sampled (with the keyboard attached, QMP keys never reach the buttons)."""
        t0, detail, woke = time.time(), "never sampled", 0
        while time.time() - t0 < timeout and self.qemu.poll() is None:
            if not self.keyboard and time.time() - t0 > 60 and time.time() - woke > 20 and \
                    "_lcdEnable: enable: 0" in open(self.serial, errors="replace").read():
                itqmp.button(self.qmp, "home")
                woke = time.time()
                time.sleep(1)
            ok, detail = self.picture()
            if ok:
                time.sleep(HOME_CONFIRM_S)
                ok, detail = self.picture("lock")
                if ok:
                    return True, "%s at t+%ds, held %ds" % (detail, time.time() - t0, HOME_CONFIRM_S)
            time.sleep(5)
        return False, "no lock screen in %ds (last: %s, qemu %s)" % (
            timeout, detail, "running" if self.qemu.poll() is None else "exited")

    def wait_mux(self, timeout=240):
        t0 = time.time()
        while time.time() - t0 < timeout:
            if "Connected to v2.0" in open(self.muxlog, errors="replace").read():
                return True
            time.sleep(2)
        return False

    def tap(self, xy):
        self.ev(*xy)
        self.ev(btn=True)
        time.sleep(0.12)
        self.ev(btn=False)

    def ev(self, x=None, y=None, btn=None):
        e = []
        if x is not None:
            e += [{"type": "abs", "data": {"axis": "x", "value": int(x * 32767 / 1024)}},
                  {"type": "abs", "data": {"axis": "y", "value": int(y * 32767 / 768)}}]
        if btn is not None:
            e += [{"type": "btn", "data": {"button": "left", "down": btn}}]
        self.qmp.cmd("input-send-event", events=e)

    def drag(self, a, b, steps=30):
        self.ev(*a)
        self.ev(btn=True)
        time.sleep(0.15)
        for i in range(1, steps + 1):
            self.ev(a[0] + (b[0] - a[0]) * i / steps, a[1] + (b[1] - a[1]) * i / steps)
            time.sleep(0.03)
        time.sleep(0.3)   # rest at the end: a release while still moving reads as a flick back
        self.ev(btn=False)

    def type(self, text):
        names = {".": ["dot"], ":": ["shift", "semicolon"], "/": ["slash"], "-": ["minus"], "_": ["shift", "minus"]}
        for ch in text:
            keys = names.get(ch, [ch])
            self.qmp.cmd("send-key", keys=[{"type": "qcode", "data": k} for k in keys])
            time.sleep(0.15)

    def stop(self):
        self.procs.stop_all()
        if os.path.exists(self.sock):
            os.unlink(self.sock)


def booted(cfg, tag, r, **kw):
    """Boot to the lock screen with usbmux attached; on failure fill r and return None."""
    b = Boot(cfg, tag, **kw)
    b.start()
    ok, detail = b.wait_lock_screen()
    if not ok:
        r.set(False, detail)
        return b, None
    if not b.wait_mux():
        r.set(False, "lock screen but usbmux never attached")
        return b, None
    return b, detail


def check_boot(cfg, r):
    b, detail = booted(cfg, "boot", r)
    try:
        if detail:
            r.set(True, detail)
    finally:
        b.stop()


def check_usbmux(cfg, r):
    b, detail = booted(cfg, "usbmux", r)
    try:
        if not detail:
            return
        v = b.run(["ideviceinfo", "-k", "ProductVersion"]).stdout.strip()
        c = b.run(["ideviceinfo", "-k", "DeviceClass"]).stdout.strip()
        r.set(v == "3.2.2" and c == "iPad", "ProductVersion %r, DeviceClass %r" % (v, c))
    finally:
        b.stop()


def afc_roundtrip(b, sizes, tag):
    """Push random files, pull them back; returns a list of mismatches."""
    bad = []
    for n in sizes:
        src, back = os.path.join(b.dir, "%s-%d.bin" % (tag, n)), os.path.join(b.dir, "%s-%d.back" % (tag, n))
        with open(src, "wb") as f:
            f.write(random.randbytes(n))
        b.run(["afcclient", "put", src, "/regress-%s-%d.bin" % (tag, n)])
        b.run(["afcclient", "get", "/regress-%s-%d.bin" % (tag, n), back])
        if not os.path.exists(back) or sha256_file(src) != sha256_file(back):
            bad.append(n)
    return bad


def check_afc(cfg, r):
    b, detail = booted(cfg, "afc", r)
    try:
        if not detail:
            return
        sizes = [1, 1000, 4097, 65535, 262143]
        bad = afc_roundtrip(b, sizes, "afc")
        r.set(not bad, "sha256 identical at %s bytes" % sizes if not bad else "mismatch at %s bytes" % bad)
    finally:
        b.stop()


def check_persist(cfg, r):
    """A file pushed over AFC survives a reboot on the same overlay.

    ponytail: the guest is killed after the file has had time to reach the NAND (update(8) syncs every
    30 s), not shut down: Hold -> slide to power off blanks the panel but never reaches the PMU power-off
    on this machine yet. Switch to the clean path when it does; the iPod check needs it because HFS keeps
    catalog updates in memory until unmount."""
    overlay = os.path.join(cfg.out, "persist", "overlay")
    marker = os.path.join(cfg.out, "persist-marker.bin")
    with open(marker, "wb") as f:
        f.write(random.randbytes(70001))
    b, detail = booted(cfg, "persist", r, overlay=overlay)
    try:
        if not detail:
            return
        b.run(["afcclient", "put", marker, "/regress-persist.bin"])
        time.sleep(45)
    finally:
        b.stop()
    b2, detail = booted(cfg, "persist2", r, overlay=overlay)
    try:
        if not detail:
            return
        back = os.path.join(b2.dir, "marker.back")
        b2.run(["afcclient", "get", "/regress-persist.bin", back])
        same = os.path.exists(back) and sha256_file(back) == sha256_file(marker)
        r.set(same, "70001-byte marker identical after a reboot on the same overlay" if same
              else "marker missing or different after reboot")
    finally:
        b2.stop()


def check_net(cfg, r):
    hits = []
    token = "regress-%d.html" % random.randrange(1 << 30)

    class H(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            hits.append((self.client_address[0], self.path))
            body = b"<html><body><h1>ipad1 regress</h1></body></html>"
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, *a):
            pass

    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), H)   # slirp maps 10.0.2.2 to host loopback
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    b, detail = booted(cfg, "net", r, keyboard=True)
    try:
        if not detail:
            return
        b.drag(UNLOCK_FROM, UNLOCK_TO)
        time.sleep(3)
        b.tap(USB_ALERT_DISMISS)   # harmless empty-space tap on the home screen when there is no alert
        time.sleep(2)
        b.tap(SAFARI_ICON)
        time.sleep(8)
        b.tap(SAFARI_ADDRESS)
        time.sleep(5)   # keys sent before the field has focus are dropped
        b.type("10.0.2.2:%d/%s" % (srv.server_address[1], token))
        time.sleep(1)
        b.shot("typed")
        b.qmp.cmd("send-key", keys=[{"type": "qcode", "data": "ret"}])
        t0 = time.time()
        while not any(p == "/" + token for _, p in hits) and time.time() - t0 < 60:
            time.sleep(1)
        b.shot("safari")
        mux = open(b.muxlog, errors="replace").read()
        if any(p == "/" + token for _, p in hits):
            r.set(True, "guest fetched /%s over en1 (usbmuxd slirp)" % token)
        else:
            r.set(False, "no GET from the guest in 60s; %s" % (
                "USB Ethernet up" if "USB Ethernet up" in mux else "USB Ethernet never came up"))
    finally:
        b.stop()
        srv.shutdown()


CHECKS = {"boot": check_boot, "usbmux": check_usbmux, "afc": check_afc, "persist": check_persist,
          "net": check_net}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--checks", default=",".join(DEFAULT_CHECKS))
    ap.add_argument("--nand", default=os.path.join(FILES, "userland/golden-pristine"))
    ap.add_argument("--kboot", default=os.path.join(FILES, "7B500/k48-kboot.bin"))
    ap.add_argument("--qemu", default=os.path.join(ROOT, "build/qemu-system-arm"))
    ap.add_argument("--usbmuxd", default=USBMUXD)
    ap.add_argument("--boot-timeout", type=int, default=600, help="hard cap per QEMU, seconds")
    ap.add_argument("--out", default=None)
    a = ap.parse_args()
    ipod.START = time.time()
    a.out = a.out or tempfile.mkdtemp(prefix="ipad1regress-")
    selected = [c for c in a.checks.split(",") if c]
    results = {c: Result(c) for c in selected}
    runnable = []
    for c in selected:
        if c in PENDING:
            results[c].skip(PENDING[c])
        elif c not in CHECKS:
            sys.exit("unknown check %s (known: %s)" % (c, ", ".join(list(CHECKS) + list(PENDING))))
        else:
            runnable.append(c)
    with concurrent.futures.ThreadPoolExecutor(MAX_QEMUS) as pool:
        futs = {pool.submit(CHECKS[c], a, results[c]): c for c in runnable}
        for f in concurrent.futures.as_completed(futs):
            try:
                f.result()
            except Exception as e:
                results[futs[f]].set(False, "harness error: %r" % e)
    print("=" * 62)
    for c in selected:
        r = results[c]
        state = "SKIP" if r.skipped else ("PASS" if r.ok else "FAIL")
        print("%-5s %-11s %s" % (state, c, r.detail))
    print("=" * 62)
    failed = [c for c in selected if results[c].ok is False]
    print("%d check(s) failed; artifacts in %s; %.1f min" % (len(failed), a.out, (time.time() - ipod.START) / 60))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
