#!/usr/bin/env python3
"""Regression gate for the ipad1 machine (iPad 1, iOS 3.2.2), the iPod suite's counterpart.

    tests/ipad1/regress.py                     # default tier
    tests/ipad1/regress.py --checks net,afc    # explicit selection
    tests/ipad1/regress.py --device DIR        # an ipad1_device.py device dir (booted through its iBoot, NOR and catalog keys)

Every check boots its own copy-on-write overlay of golden-pristine (the base is never written), with
usbmuxd-qemu's ipad1 build as the USB host where the check talks USB (otherwise the machine's built-in
host). Checks run in parallel, each on its own QEMU.

  boot     lock screen on the panel: lit and a picture (many colours), not a solid fill; then, unlocked with
           the USB keyboard attached, no stock "USB device not supported" alert and Hold locks the panel
           (--guest-package DIR: it_boot must report the offered serial; a package installed this boot
           means one more boot on the same overlay, the first one's mounter ran the old shim)
           and the GL bridge refused nothing on the way (gles-rejects), nothing painted magenta.
           5.x (the device lock's build): what a 5.x boot shows instead. Lit; it_boot's report (a device
           with a guest package: its console line naming the lock's seed, or with --guest-package as
           above); lockdown answering over usbmux (ProductVersion, ActivationState Activated when the
           device has an activation hook); after the slide, Setup Assistant's first page (a fresh device)
           or the home screen (a device past Setup), read off the screen (tests/ipad1/ocr.swift) and, with
           it_agent up, the frontmost app agreeing; no alert over it; Hold locks the panel; GL clean
  gles     SpringBoard's compositor through the GL bridge (see check_gles); on 5.x a fresh device walks
           Setup Assistant first, page by page as the screen shows them

  usbmux   ideviceinfo over the bridge answers ProductVersion (the store's device.lock.json, else
           3.2.2), DeviceClass iPad
  afc      push and pull files at sizes that are not multiples of 512, SHA-256 identical
  persist  a file pushed over AFC survives a reboot on the same overlay (see check_persist)
  wifi     the BCM4329 comes up, joins the model's open "qemu-ios" BSS and takes a DHCP lease (serial)
  wifi-early  the same join 10 ms after the model arms it (IT_WIFI_AUTOJOIN=0.01), the order a loaded host
           produces: lock screen, joined, no kernel panic (a join ahead of the driver's interface panics 4.3)
  net      the default network, Wi-Fi (BCM4329 on the machine's slirp netdev): Safari, typed on the
           emulated USB keyboard, fetches a page from a host HTTP server at 10.0.2.2
  audio    tests/ipad1/audio-check.py: boot sound, unlock, lock, unlock correlate with the originals
  prefs    (opt-in) a device it_prefs has not run on before: Brightness at maximum and Auto-Lock Never,
           read back over the agent, and its once-only marker (see check_prefs)
  nocharge (opt-in) usb-charger=off over the bridge: connected, not charge-capable, not charging; AFC works
  net-usb  (opt-in) the same fetch over USB Ethernet: en1, usbmuxd's slirp, it_ethlink in the image
  shadow   (opt-in) Safari's Bookmarks popover casts a soft drop shadow (an A008 surface), not a solid box
  appinstall, applaunch
           SKIP: stock installd rejects apps not validly signed for this device

Exits non-zero if any selected check FAILs.
"""
import argparse
import concurrent.futures
import http.server
import hashlib
import json
import importlib.util
import os
import plistlib
import random
import re
import shutil
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
itqmp.W, itqmp.H = 1024, 768   # scanout pixels, for itqmp.move/button users (audio-check)
Result, Procs, free_port, sha256_file, log = ipod.Result, ipod.Procs, ipod.free_port, ipod.sha256_file, ipod.log

FILES = os.path.expanduser("~/Developer/qemu-ios-files/ipad1")
USBMUXD = os.path.expanduser("~/Developer/usbmuxd-qemu-ipad1-net/src/usbmuxd")
DEFAULT_CHECKS = ["boot", "gles", "usbmux", "afc", "persist", "wifi", "wifi-early", "net", "audio"]
PENDING = {"appinstall": "use `app` (install + launch; needs a device whose recipe installed AppSync)",
           "applaunch": "use `app`"}
HARNESS_IPA = os.path.join(ROOT, "contrib/it-harness/build/Harness.ipa")   # contrib/it-harness/build.sh
# Scanout is 1024x768 with the portrait UI turned on it. The boot logo is a small Apple on black (a few %
# lit); the lock screen is a full wallpaper (~99% lit, unlike the iPod's dark panel). A stalled panel's
# solid fill is also fully lit, so the frame must also be a picture: many distinct colours.
LIT_MIN_FRACTION, MIN_COLOURS, HOME_CONFIRM_S = 0.5, 64, 2
# Panel coordinates of the upright portrait UI (interface 1, the default accel-orientation): the portrait
# top (status bar) is the panel's left edge, portrait left its bottom edge.
UNLOCK_FROM, UNLOCK_TO = (959, 477), (959, 47)
SAFARI_ICON, SAFARI_ADDRESS = (959, 650), (55, 437)
# The USB keyboard trips MobileStorageMounter's stock "not supported" alert on every boot (3.2.x at once,
# 4.2.1 a few seconds after unlock), and while it is up SpringBoard never auto-locks. The guest package's
# it_msmquiet shim drops it inside the mounter and says so on the console (docs/ipad1/usb-keyboard.md).
MSM_QUIET = 'it_msmquiet: hid the USB "not supported" notice'
LOCK_S = 8                  # Hold -> panel off, at most; with the alert up it lit again at once
DARK_MAX_FRACTION = 0.05    # the scanout with the panel off

sys.path.insert(0, os.path.join(ROOT, "imgtools"))
import ipad1_boot
sys.path.insert(0, os.path.join(ROOT, "tests"))
import frame_reference
import framecheck  # noqa: E402  (the audit's frame-reference check)
GLES_REFS = os.path.join(ROOT, "tests", "gles-refs")

launch_lock = threading.Lock()


class Boot:
    """One QEMU on an overlay of the golden store, plus (usb=True) the usbmuxd bridge; without it the
    machine's built-in USB host configures the device. Wi-Fi (the BCM4329 on a slirp netdev the machine
    creates) is the machine's default; wifi=False turns it off. wav records the audio out."""
    n = 0

    def __init__(self, cfg, tag, overlay=None, keyboard=False, usb=True, wifi=True, wav=None, extra=(), env=None,
                 machine=""):
        Boot.n += 1
        self.machine_extra = machine
        self.qemu_env = dict(os.environ, **env) if env else None
        self.cfg, self.tag, self.keyboard, self.extra = cfg, tag, keyboard, list(extra)
        self.usb, self.wifi, self.wav = usb, wifi, wav
        self.dir = os.path.join(cfg.out, tag)
        os.makedirs(self.dir, exist_ok=True)
        # A default overlay is a fresh device: drop one a previous run left under the same out dir,
        # or a rerun boots the old run's NAND (an app "installed" twice is an upgrade). Callers that
        # mean to reuse state (persist, snapshots) pass overlay= explicitly.
        if overlay is None:
            shutil.rmtree(os.path.join(self.dir, "overlay"), ignore_errors=True)
        self.overlay = overlay or os.path.join(self.dir, "overlay")
        self.procs = Procs()
        self.sock = "/tmp/ipad1rg-%d-%d.qmp" % (os.getpid(), Boot.n)   # sun_path < 104
        self.serial = os.path.join(self.dir, "serial.log")
        self.muxlog = os.path.join(self.dir, "usbmuxd.log")
        self.qmp = self.qemu = None
        self.udid = getattr(cfg, "udid", None)

    def start(self):
        cfg = self.cfg
        with launch_lock:       # free ports are claimed one boot at a time
            machine = "%s,%s,nand=%s,nand-overlay=%s" % (   # a writable NOR copy lives in the overlay dir
                getattr(cfg, "machine", "ipad1"), ipad1_boot.boot_options(cfg, self.overlay), cfg.nand, self.overlay)
            self.usb_port = self.mux_port = 0
            if self.usb:
                self.usb_port = free_port(21300, 21399)
                self.mux_port = free_port(27400, 27499)
                env = dict(os.environ, USBMUXD_QEMU_ADDR="127.0.0.1:%d" % self.usb_port, USBMUXD_QEMU_DELAY="0")
                os.makedirs(os.path.join(self.dir, "conf"), exist_ok=True)
                self.procs.spawn([cfg.usbmuxd, "-f", "-v", "-v", "-S", "127.0.0.1:%d" % self.mux_port,
                                  "-P", "NONE", "-C", os.path.join(self.dir, "conf")], self.muxlog, env=env)
                machine += ",usb-tcp-addr=127.0.0.1:%d" % self.usb_port
            if getattr(cfg, "guest_package", None):
                machine += ",guest-package=" + cfg.guest_package
            if os.environ.get("IPAD1_MACHINE_EXTRA"):   # e.g. iop-core=off, as boot-smoke.py takes it
                machine += "," + os.environ["IPAD1_MACHINE_EXTRA"]
            if self.machine_extra:
                machine += "," + self.machine_extra
            # What the GL bridge refuses is painted magenta and counted (gl_clean below).
            machine += ",gles-debug=on"
            argv = ["timeout", str(cfg.boot_timeout), cfg.qemu, "-machine", machine + ("" if self.wifi else ",wifi=off"),
                    "-display", "none", "-monitor", "none", "-serial", "file:" + self.serial,
                    "-qmp", "unix:%s,server,nowait" % self.sock]
            argv += ["-audio", "driver=wav,path=" + self.wav] if self.wav else ["-audio", "driver=none"]
            # A USB keyboard takes QMP keys ahead of the machine's button chords, so only boots that type get one.
            # max-power=20: 4.x gives the dock port's host side a small budget (the arbitrator's
            # AAPL,power-supply) and refuses the default 100 mA keyboard; 3.x never checks.
            argv += ["-device", "usb-kbd,bus=usb-bus.0,max-power=20"] if self.keyboard else []
            argv += self.extra
            self.qemu = self.procs.spawn(argv, os.path.join(self.dir, "qemu.log"), env=self.qemu_env)
            time.sleep(2)
        self.qmp = itqmp.QMP(self.sock, timeout=60)
        log("%s: qemu pid %d, usb %d, mux %d" % (self.tag, self.qemu.pid, self.usb_port, self.mux_port))

    def env(self):
        return dict(os.environ, USBMUXD_SOCKET_ADDRESS="127.0.0.1:%d" % self.mux_port)

    def run(self, argv, timeout=120):
        if self.udid and argv[0] in ("afcclient", "ideviceinfo", "idevicepair", "ideviceinstaller") and "-u" not in argv:
            argv = [argv[0], "-u", self.udid] + argv[1:]
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

    def lit(self, name="screen"):
        """Fraction of lit samples: ~1 on the lock or home screen, ~0 with the panel off."""
        w, h, pix = itqmp.read_ppm(self.shot(name))
        return sum(1 for v in pix[::7] if v > ipod.LIT_THRESHOLD) / len(pix[::7])

    def guest_package_status(self, timeout=0):
        """The machine's guest-package-status: "report SERIAL RESULT (N this boot) ..." once it_boot has run."""
        t0 = time.time()
        while True:
            status = self.qmp.cmd("qom-get", path="/machine", property="guest-package-status")
            if status.startswith("report ") or time.time() - t0 >= timeout:
                return status
            time.sleep(2)

    def wait_lock_screen(self, timeout=300):
        """(ok, detail): lit, not solid, and still so HOME_CONFIRM_S later.

        The lock screen turns the panel off about 8 s after it appears, so the confirmation is short and
        callers act right after. Home (the button-home property, which works even while a USB keyboard
        owns QMP keys) wakes a panel that went dark before it was sampled."""
        t0, detail, woke = time.time(), "never sampled", 0
        while time.time() - t0 < timeout and self.qemu.poll() is None:
            if time.time() - t0 > 60 and time.time() - woke > 20 and \
                    "_lcdEnable: enable: 0" in open(self.serial, errors="replace").read():
                self.press("home")
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
        # A "Connected to v2.0" log line predates pairing and can outlive a
        # disconnected device. Require a live lockdown query on this bridge.
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline and self.qemu.poll() is None:
            try:
                if not self.udid:
                    listing = self.run(["idevice_id", "-l"], timeout=5)
                    devices = listing.stdout.split()
                    if listing.returncode == 0 and len(devices) == 1:
                        self.udid = devices[0]
                if self.udid:
                    info = self.run(["ideviceinfo", "-k", "ProductVersion"], timeout=5)
                    if info.returncode == 0 and info.stdout.strip() == self.cfg.product_version:
                        return True
            except subprocess.TimeoutExpired:
                pass
            time.sleep(1)
        return False

    def press(self, button, hold=0.3):
        """Hold or Home through the machine's button-* properties (a4-touch)."""
        for v in (True, False):
            self.qmp.cmd("qom-set", path="/machine", property="button-" + button, value=v)
            time.sleep(hold if v else 0)

    def tap(self, xy):
        self.ev(*xy)
        self.ev(btn=True)
        time.sleep(0.12)
        self.ev(btn=False)

    def ev(self, x=None, y=None, btn=None):
        e = []
        if x is not None:
            e += [{"type": "abs", "data": {"axis": "x", "value": int(x * 32767 / itqmp.W)}},
                  {"type": "abs", "data": {"axis": "y", "value": int(y * 32767 / itqmp.H)}}]
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

    def powerdown(self):
        try:
            return itqmp.guest_powerdown(self.qmp, self.qemu, self.tag, log, charging_halt=True)
        finally:
            self.qmp = None

    def stop(self):
        self.procs.stop_all()
        if os.path.exists(self.sock):
            os.unlink(self.sock)


def battery_info(b):
    """lockdown's com.apple.mobile.battery domain: {key: value as text}."""
    out = b.run(["ideviceinfo", "-q", "com.apple.mobile.battery"]).stdout
    return dict(l.split(": ", 1) for l in out.splitlines() if ": " in l)


def booted(cfg, tag, r, **kw):
    """Boot to the lock screen with usbmux attached; on failure fill r and return None."""
    b = Boot(cfg, tag, **kw)
    b.start()
    ok, detail = b.wait_lock_screen()
    if not ok:
        r.set(False, detail)
        return b, None
    if b.usb and not b.wait_mux():
        r.set(False, "lock screen but usbmux never attached")
        return b, None
    return b, detail


MAGENTA_MAX = 0.001     # of the frame: gles-debug's paint is a layer's worth, never a stray pixel


def qualified_frame_reference(cfg, scene):
    """Use the shared build-scoped independent reference contract."""
    return frame_reference.qualified(GLES_REFS, "k48ap", getattr(cfg, "build", None),
                                     cfg.product_version, scene)


def gl_clean(b, r, detail, shots=(), require_refs=()):
    """Judge refusal counters, magenta paint and build-qualified frame pictures.

    The dedicated gles check requires a home reference. Other checks still
    report their independent liveness/counter evidence accurately when their
    captured scene has no qualified reference.
    """
    rejects = itqmp.gles_rejects(b.qmp)
    magenta = 0.0 if getattr(b.cfg, "gl_test", False) else max([itqmp.magenta_fraction(s, step=4) for s in shots] or [0.0])
    bad, compared = None, []
    if not getattr(b.cfg, "gl_test", False):
        for shot in shots:
            name = os.path.splitext(os.path.basename(shot))[0]
            if name not in ("home", "screen", "lock"):
                continue
            reference, why = qualified_frame_reference(b.cfg, name)
            if why:
                if name in require_refs:
                    bad = (name, why)
                    break
                continue
            verdict = framecheck.verdict(shot, reference)
            if not verdict["ok"]:
                bad = (name, verdict["why"])
                break
            compared.append(name)
        if not bad:
            for name in require_refs:
                if name not in compared:
                    bad = (name, "required scene was not compared to a qualified reference")
                    break
    # A software CoreAnimation draws the same pictures and refuses nothing (4.3.x did, while the old GLI shim
    # lost GL), so only the GL front end's own line proves CoreAnimation took the GL path.
    serial = open(b.serial, errors="replace").read() if os.path.exists(b.serial) else ""
    if "CoreAnimation composites through the host" not in serial:
        r.set(False, "%s; SpringBoard composited in software CoreAnimation (no GL-path line from the front end)" % detail)
    elif rejects:
        r.set(False, "%s; the GL bridge refused %d thing(s): %s" % (
            detail, len(rejects), ", ".join("%s x%d" % kv for kv in sorted(rejects.items()))))
    elif magenta > MAGENTA_MAX:
        r.set(False, "%s; gles-debug painted %.2f%% of a screen magenta (a refusal the counters missed)" % (
            detail, magenta * 100))
    elif bad:
        r.set(False, "%s; frame reference %s: %s" % (detail, bad[0], bad[1]))
    else:
        r.set(True, "%s; GL bridge refused nothing; %s" % (detail,
              "qualified frame-ref " + ", ".join(compared) if compared else "no frame-reference comparison for these scenes"))
    return r.ok


def check_boot(cfg, r):
    """Lock screen, then unlocked with the USB keyboard attached: no stock USB alert, and Hold locks the panel."""
    b, detail = booted(cfg, "boot", r, keyboard=True)
    try:
        if not detail:
            return
        pkg = ""
        if cfg.guest_package:
            offered = [l.split()[1] for l in open(os.path.join(cfg.guest_package, "offer")) if l.startswith("serial ")][0]
            status = b.guest_package_status(timeout=60)
            if not status.startswith("report "):
                return r.set(False, "%s, but no it_boot report: %s" % (detail, status))
            serial, result = status.split()[1:3]
            if serial != offered:
                return r.set(False, "%s, but the loader did not take serial %s: %s" % (detail, offered, status))
            pkg = "; loader reports serial %s" % serial
            if result in ("1", "2"):
                # This boot moved to the offer, but the mounter had loaded the previous package's shim before
                # it_boot replaced it (a respring does not drop the alert it let through): boot again.
                b.stop()
                b, detail = booted(cfg, "boot2", r, keyboard=True, overlay=b.overlay)
                if not detail:
                    return
                pkg = "; loader installed serial %s, boot 2 %s" % (serial, b.guest_package_status(timeout=60).split(" (")[0])
        if cfg.major >= 5:
            return check_boot_5(cfg, r, b, detail + pkg)
        for attempt in range(2):
            if b.lit("pre-unlock") < LIT_MIN_FRACTION:   # waiting for usbmux outlasted the lock screen's panel
                b.press("home")
                time.sleep(1.5)
            b.drag(UNLOCK_FROM, UNLOCK_TO)   # at once: the lock screen dims about 8 s after it appears
            time.sleep(10)                   # 4.2.1's alert would be up by now
            ok, home = b.picture("home")
            if ok:
                break
            b.press("home")                  # a slide the dimmed panel swallowed: wake it and slide again
            b.wait_lock_screen(60)
        if not ok:
            return r.set(False, "unlock failed: %s 10 s after the slide" % home)
        # The iPod raises no "not supported" alert for the keyboard (nothing for the shim to hide).
        if getattr(cfg, "machine", "ipad1") == "ipad1" and MSM_QUIET not in open(b.serial, errors="replace").read():
            return r.set(False, "the mounter never reported hiding the USB alert (see %s/home.ppm)" % os.path.basename(b.dir))
        locked = hold_locks(b)
        if not locked.startswith("Hold locked"):
            return r.set(False, locked)
        hid = "shim hid the USB alert, " if getattr(cfg, "machine", "ipad1") == "ipad1" else ""
        gl_clean(b, r, "%s; unlocked, %s%s%s" % (detail, hid, locked, pkg), [b.shot("boot-gl")])
    finally:
        b.stop()


def hold_locks(b):
    """Press Hold: "Hold locked the panel in N s" if it goes dark and stays so, else what went wrong. The lock
    button is what the USB alert used to defeat (Hold blanked the panel and it lit again at once)."""
    b.press("hold")
    t0, lit = time.time(), 1.0
    while time.time() - t0 < LOCK_S and lit > DARK_MAX_FRACTION:
        time.sleep(1)
        lit = b.lit("dark")
    if lit > DARK_MAX_FRACTION:
        return "Hold did not lock: panel still lit (%.0f%%) %d s later (an alert?)" % (lit * 100, LOCK_S)
    took = time.time() - t0
    time.sleep(3)   # ... and it stays dark: the alert used to relight it at once
    if b.lit("dark") > DARK_MAX_FRACTION:
        return "Hold locked the panel but it lit again within 3 s (an alert?)"
    return "Hold locked the panel in %d s" % took


def check_boot_5(cfg, r, b, detail):
    """check_boot on 5.x, which neither shows the stock USB alert nor, fresh, a home screen: it_boot's report,
    lockdown answering, then Setup Assistant (fresh) or the home screen (past Setup) after the slide, nothing
    over it, Hold locks, GL clean."""
    if cfg.package_seed is not None and not cfg.guest_package:
        # no offer this boot, so no QC report: it_boot says which package it loaded on the console
        said = "it_boot: package %s\n" % cfg.package_seed
        if said not in open(b.serial, errors="replace").read():
            return r.set(False, "%s, but it_boot never said it loaded the device's package (%r)" % (detail, said.strip()))
        detail += "; it_boot loaded package %s" % cfg.package_seed
    if b.usb:
        v = b.run(["ideviceinfo", "-k", "ProductVersion"]).stdout.strip()
        act = b.run(["ideviceinfo", "-k", "ActivationState"]).stdout.strip()
        if v != cfg.product_version or not act or (cfg.activated and act != "Activated"):
            return r.set(False, "%s, but lockdown answered ProductVersion %r (want %s), ActivationState %r%s" % (
                detail, v, cfg.product_version, act, " (the device has an activation hook)" if cfg.activated else ""))
        detail += "; lockdown %s %s" % (v, act)
    ok, why = slide_open(b, "the lock screen")
    if not ok:
        return r.set(False, "%s; %s" % (detail, why))
    region_settled(b, TITLE)
    ppm = b.shot("opened")
    found, front = ocr(ppm), frontmost(b)
    if "English" in found and page_title(found) is None and front in (None, "com.apple.purplebuddy"):
        shown = "Setup Assistant's first page"
    elif "Safari" in found and front in (None, "com.apple.springboard"):
        shown = "the home screen"
    else:
        return r.set(False, "%s; after the slide neither Setup Assistant nor the home screen (frontmost %s): %s" % (
            detail, front, sorted(found)[:12]))
    if alert_up(ppm) or any("support" in t.lower() for t in found):
        return r.set(False, "%s; an alert over %s: %s" % (detail, shown, sorted(found)[:12]))
    locked = hold_locks(b)
    if not locked.startswith("Hold locked"):
        return r.set(False, "%s; %s: %s" % (detail, shown, locked))
    gl_clean(b, r, "%s; %s%s, no alert, %s" % (detail, shown, " (frontmost %s)" % front if front else "", locked),
             [ppm])


OCR_SRC, OCR_BIN = os.path.join(HERE, "ocr.swift"), os.path.join(ROOT, "build", "ipad1-ocr")
ocr_lock = threading.Lock()


def ocr(ppm):
    """{text: (panel x, y) of its centre} for the text on a screendump, read upright by Vision
    (tests/ipad1/ocr.swift, built here once). Of two pieces with the same text, the one nearer the top."""
    with ocr_lock:
        if not os.path.exists(OCR_BIN) or os.path.getmtime(OCR_BIN) < os.path.getmtime(OCR_SRC):
            os.makedirs(os.path.dirname(OCR_BIN), exist_ok=True)
            fd, staged = tempfile.mkstemp(prefix="ipad1-ocr-", dir=os.path.dirname(OCR_BIN))
            os.close(fd)
            try:
                compile = subprocess.run(["swiftc", "-O", "-module-cache-path", os.path.join(os.path.dirname(OCR_BIN), "swift-modules"), OCR_SRC, "-o", staged], capture_output=True, text=True)
                if compile.returncode:
                    raise RuntimeError("OCR compiler failed: " + compile.stderr)
                os.replace(staged, OCR_BIN)
            finally:
                if os.path.exists(staged): os.unlink(staged)
    found = {}
    for attempt in range(3):   # Vision's recognizer sometimes fails to build its compute plan on a loaded host
        run = subprocess.run([OCR_BIN, ppm], capture_output=True, text=True)
        if run.returncode == 0 or attempt == 2:
            run.check_returncode()
            break
        time.sleep(2)
    lines = [l.split(" ", 4) for l in run.stdout.splitlines()]
    for x0, y0, x1, y1, text in sorted(lines, key=lambda l: int(l[1]), reverse=True):
        # upright portrait -> panel: the portrait top is the panel's left edge, the portrait left its bottom
        found[text.strip()] = ((int(y0) + int(y1)) // 2, 767 - (int(x0) + int(x1)) // 2)
    return found


def page_title(found):
    """The navigation bar's title: text across the portrait top (panel x 25..65) away from its
    Back/Next buttons (panel y 150..620); None on a page without one (Setup's language list, home)."""
    return next((t for t, (x, y) in found.items() if 25 <= x <= 65 and 150 <= y <= 620), None)


# iOS 5's Setup Assistant, which every fresh 5.x device opens behind its "slide to set up" lock screen. The
# pages it shows depend on the device's state (without a Wi-Fi join it skips the Apple ID page; with an
# activation hook it never shows activation), so the walk reads each page off the screen (its title) and
# answers it by label: the pick below if the page has one, then its Next. The language page has no title
# and its Next is an unlabelled arrow. An alert (navy buttons in ALERT) is answered by the first of
# ALERT_YES it offers. "Start Using iPad" ends it.
SETUP_PICKS = {"language": "English", "Country or Region": "Australia",
               "Location Services": "Disable Location Services", "Set Up iPad": "Set Up as New iPad",
               "Apple ID": "Skip This Step", "Terms and Conditions": "Agree", "Diagnostics": "Don't Send",
               "Thank You": "Start Using iPad"}
ALERT_YES = ("Skip", "Agree", "Continue", "OK", "Disable")
NEXT_ARROW = (42, 28)
TITLE, ALERT = (20, 150, 65, 620), (548, 255, 605, 515)


def alert_up(ppm):
    """An alert's navy buttons fill the ALERT column (~30% of it blue); no Setup page is blue there."""
    w, h, pix = itqmp.read_ppm(ppm)
    x0, y0, x1, y1 = ALERT
    pts = [(pix[(y * w + x) * 3], pix[(y * w + x) * 3 + 2]) for y in range(y0, y1, 4) for x in range(x0, x1, 4)]
    return sum(1 for r, b in pts if b > r + 40 and b > 80) > 0.15 * len(pts)


def region(ppm, box):
    w, h, pix = itqmp.read_ppm(ppm)
    x0, y0, x1, y1 = box
    return b"".join(bytes(pix[(y * w + x0) * 3:(y * w + x1) * 3]) for y in range(y0, y1))


def region_settled(b, box, timeout=20):
    """The box's pixels once they hold still for a second (a page still sliding in under load)."""
    last, t0 = region(b.shot("wait"), box), time.time()
    while time.time() - t0 < timeout:
        time.sleep(1)
        now = region(b.shot("wait"), box)
        if now == last:
            break
        last = now
    return last


SLIDER = (930, 250, 990, 520)       # the "slide to set up" track, portrait bottom


def slide_open(b, what):
    """Slide the lock screen's knob (again, if a drag under load missed) until the lock screen is gone."""
    for attempt in range(4):
        if b.lit("pre-slide") < LIT_MIN_FRACTION:     # the lock screen darkens its panel ~8 s after waking
            b.press("home")
            time.sleep(1.5)
        ref = region(b.shot("pre-slide"), SLIDER)
        b.drag(UNLOCK_FROM, UNLOCK_TO)
        time.sleep(3)
        if b.lit("slid") >= LIT_MIN_FRACTION and region(b.shot("wait"), SLIDER) != ref:
            return True, None
    return False, "%s: the slide never opened it" % what


def setup_assistant_5(b):
    """From Setup's lock screen, slide to set up and walk whatever pages it shows; (ok, detail)."""
    ok, why = slide_open(b, "Setup Assistant")
    if not ok:
        return False, why
    pages, seen = [], {}
    for step in range(40):
        region_settled(b, TITLE)
        ppm = b.shot("setup-%02d" % step)
        found = ocr(ppm)
        if alert_up(ppm):
            yes = next((t for t in ALERT_YES if t in found and ALERT[0] <= found[t][0] <= ALERT[2]), None)
            if not yes:
                return False, "Setup Assistant: an alert with none of %s: %s" % (ALERT_YES, sorted(found))
            b.tap(found[yes])
            t0 = time.time()
            while time.time() - t0 < 30 and alert_up(b.shot("wait")):
                time.sleep(1)
            if pages:
                pages[-1] += " (%s)" % yes
            continue
        page = page_title(found) or ("language" if "English" in found else None)
        if page is None:
            return False, "Setup Assistant: after %s, a page it does not know: %s" % (
                ", ".join(pages) or "the slide", sorted(found)[:12])
        seen[page] = seen.get(page, 0) + 1
        if seen[page] > 3:
            return False, "Setup Assistant: the %s page did not move on: %s" % (page, sorted(found)[:12])
        if seen[page] == 1:
            pages.append(page)
        pick = SETUP_PICKS.get(page)
        ref = region(ppm, TITLE)
        if pick in found:
            b.tap(found[pick])
            if pick == "Start Using iPad":
                return wait_home_5(b, "Setup Assistant walked: %s" % ", ".join(pages))
            time.sleep(2)
            if alert_up(b.shot("wait")):
                continue
        nxt = found.get("Next", NEXT_ARROW if page == "language" else None)
        if nxt:
            b.tap(nxt)
        t0 = time.time()       # the next page (its title) or an alert
        while time.time() - t0 < 60:
            now = b.shot("wait")
            if region(now, TITLE) != ref or alert_up(now):
                break
            time.sleep(1)
    return False, "Setup Assistant: still walking after 40 pages: %s" % ", ".join(pages)


def wait_home_5(b, detail, timeout=60):
    """(ok, detail) once the home screen is up: its dock's Safari on the screen and, with it_agent up,
    SpringBoard frontmost (not Setup, not the lock screen)."""
    t0, found, front = time.time(), {}, ""
    while time.time() - t0 < timeout:
        found = ocr(b.shot("home-wait"))
        front = frontmost(b)
        if "Safari" in found and front in (None, "com.apple.springboard"):
            return True, detail
        time.sleep(3)
    return False, "%s, but no home screen %ds later (frontmost %s): %s" % (detail, timeout, front, sorted(found)[:12])


def frontmost(b):
    """The frontmost app's bundle id (it_agent), "Lock Screen" when SpringBoard says so; None without the agent."""
    if not itqmp.agent_alive(b.qmp):
        return None
    status, out = itqmp.agent(b.qmp, "frontmost")
    lines = out.decode(errors="replace").split("\n")
    return "Lock Screen" if lines[1:2] == ["Lock Screen"] else (lines[0] if status == 0 else "error %d" % status)


def check_gles(cfg, r):
    """The GL bridge under SpringBoard's own compositor: lock screen, home screen, a page swipe, Safari;
    nothing refused, nothing painted magenta. On 5.x a fresh device first walks the Setup Assistant
    (setup_assistant_5, page by page; the device needs an activation hook), swipes to Spotlight and back, and closes Safari. With a --gl-test device, tests/ipad1/gltest.py's fixture
    scene as well (its readback, colour census and counters)."""
    if getattr(cfg, "gl_test", False):
        # The fixture job covers SpringBoard's screens from 12 s into every boot, so on such a
        # device the fixture IS the gles leg: its readback and colour census, and the counters.
        p = subprocess.run([sys.executable, os.path.join(HERE, "gltest.py"), os.path.dirname(os.path.abspath(cfg.nand)),
                            "--qemu", cfg.qemu, "--out", os.path.join(cfg.out, "gltest")], capture_output=True, text=True)
        lines = p.stdout.strip().splitlines()
        summary = "; ".join(l.strip() for l in lines if any(k in l for k in ('"readback"', '"rejects"', 'present fps')))
        return r.set(p.returncode == 0, "gltest.py %s: %s" % ((lines or ["(no output)"])[-1], summary or p.stderr.strip()[-200:]))
    # 5.x's Setup with a way to the internet fetches mesu's SoftwareUpdate catalog (7 MB today) and talks TLS
    # to Apple's current servers, and then its Apple ID page ignores "Skip This Step" for minutes on end
    # (guest idle, nothing on the wire): the walk goes without internet, the BSS joined and leased
    # (restrict=on), and Setup takes its no-network path (Continue without Wi-Fi, no Apple ID page).
    offline = ["-netdev", "user,id=wifi0,restrict=on"] if cfg.major >= 5 else []
    b, detail = booted(cfg, "gles", r, keyboard=True, extra=offline)
    try:
        if not detail:
            return
        if b.lit("pre-unlock") < LIT_MIN_FRACTION:   # waiting for usbmux outlasted the lock screen's panel
            b.press("home")
            time.sleep(2)
        five = cfg.major >= 5
        if not five:
            b.drag(UNLOCK_FROM, UNLOCK_TO)
        else:
            ok, walked = setup_assistant_5(b)
            if not ok:
                return r.set(False, "%s; %s" % (detail, walked))
        time.sleep(10)                   # the mounter's shim hides the USB alert; let it settle
        shots = [b.shot("home")]
        if five:                             # 5.x's one app page: Spotlight's page and back
            b.drag((511, 617), (511, 87))
            time.sleep(3)
            shots.append(b.shot("spotlight"))
            b.drag((511, 87), (511, 617))
        else:
            b.drag((511, 87), (511, 617))    # next home page (portrait right-to-left)
        time.sleep(2)
        shots.append(b.shot("home2"))
        if not five:                         # (5.x: Home on the first page opens Spotlight)
            b.press("home")
            time.sleep(2)
        b.tap(SAFARI_ICON)
        time.sleep(8)
        shots.append(b.shot("safari"))
        if five:                             # and closed again
            b.press("home")
            time.sleep(3)
            shots.append(b.shot("closed"))
        gl_clean(b, r, "%slock, home, %s" % (walked + ", " if five else "",
                                             "Spotlight, Safari opened and closed" if five else "page 2, Safari"), shots, require_refs=("home",))
    finally:
        b.stop()


def check_usbmux(cfg, r):
    b, detail = booted(cfg, "usbmux", r)
    try:
        if not detail:
            return
        v = b.run(["ideviceinfo", "-k", "ProductVersion"]).stdout.strip()
        c = b.run(["ideviceinfo", "-k", "DeviceClass"]).stdout.strip()
        want = ipad1_boot.PORTRAIT.get(getattr(cfg, "machine", "ipad1"), "iPad")
        r.set(v == cfg.product_version and c == want, "ProductVersion %r, DeviceClass %r" % (v, c))
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
    """Verify the write, require guest shutdown, then compare after reboot."""
    overlay = os.path.join(cfg.out, "persist", "overlay")
    marker = os.path.join(cfg.out, "persist-marker.bin")
    with open(marker, "wb") as f:
        f.write(random.randbytes(70001))
    b, detail = booted(cfg, "persist", r, overlay=overlay)
    try:
        if not detail:
            return
        put = b.run(["afcclient", "put", marker, "/regress-persist.bin"])
        before = os.path.join(b.dir, "marker.before")
        get = b.run(["afcclient", "get", "/regress-persist.bin", before])
        if put.returncode or get.returncode or not os.path.exists(before) or sha256_file(before) != sha256_file(marker):
            r.set(False, "AFC marker write/readback failed before shutdown: " + put.stderr + get.stderr)
            return
        if not b.powerdown():
            r.set(False, "guest shutdown not confirmed; persistence cannot be judged")
            return
    finally:
        b.stop()
    b2, detail = booted(cfg, "persist2", r, overlay=overlay)
    try:
        if not detail:
            return
        back = os.path.join(b2.dir, "marker.back")
        b2.run(["afcclient", "get", "/regress-persist.bin", back])
        same = os.path.exists(back) and sha256_file(back) == sha256_file(marker)
        r.set(same, "70001-byte marker identical after guest shutdown and reboot on the same overlay" if same
              else "marker missing or different after reboot")
    finally:
        b2.stop()


def safari_fetch(cfg, r, tag, via, **kw):
    """Safari, typed on the emulated USB keyboard, fetches a page from a host HTTP server at 10.0.2.2
    (slirp's gateway, mapped to host loopback). via names the path for the report."""
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
    b, detail = booted(cfg, tag, r, keyboard=True, **kw)
    try:
        if not detail:
            return
        b.drag(UNLOCK_FROM, UNLOCK_TO)
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
        if any(p == "/" + token for _, p in hits):
            r.set(True, "guest fetched /%s over %s" % (token, via))
        else:
            why = ""
            if b.usb:
                mux = open(b.muxlog, errors="replace").read()
                why = "; USB Ethernet %s" % ("up" if "USB Ethernet up" in mux else "never came up")
            r.set(False, "no GET from the guest in 60s%s" % why)
    finally:
        b.stop()
        srv.shutdown()


def check_net(cfg, r):
    """The default network: the BCM4329 on a QEMU slirp netdev (wifi=on), built-in USB host."""
    safari_fetch(cfg, r, "net", "Wi-Fi en0 (QEMU slirp)", usb=False)


def check_net_usb(cfg, r):
    """Opt-in: USB Ethernet, en1 on usbmuxd's slirp (needs it_ethlink in the image)."""
    safari_fetch(cfg, r, "net-usb", "USB Ethernet en1 (usbmuxd slirp)", wifi=False)


def check_wifi(cfg, r):
    """Stock AppleBCMWLAN joins the model's open BSS and takes a lease (a4-guest; docs/ipad1/wifi.md)."""
    b, detail = booted(cfg, "wifi", r, usb=False)
    try:
        if not detail:
            return
        t0, text = time.time(), ""
        while time.time() - t0 < 120:
            text = open(b.serial, errors="replace").read()
            if "receivedIPv4Address(): Received" in text:   # 3.2.2 "... IP Address", 4.2.1 "... address A.B.C.D"
                break
            time.sleep(2)
        joined = 'ssid[ 8] = "qemu-ios"' in text
        leased = "receivedIPv4Address(): Received" in text
        fw = "BCM4329 revision B1" in text and "initFirmware(): successful initialization" in text
        if joined and leased and fw:
            r.set(True, "BCM4329 B1 up, joined qemu-ios, DHCP lease")
        else:
            r.set(False, "firmware=%s joined=%s lease=%s" % (fw, joined, leased))
    finally:
        b.stop()


def check_wifi_early(cfg, r):
    """The auto-join 10 ms after the model arms it: what a loaded host does to the default 10 s.

    A join the model reports before AppleBCMWLAN has attached its IO80211Interface panics the guest in
    AppleBCMWLAN::setLinkState (4.3 8F190: fault_addr 0xc4, pc 0x80656f7c; LightTouchMac matrix 09-29, 8F190
    and 8G4 under host load), so the boot never reaches the lock screen. No lease is asked for: 5.x's
    IPConfiguration can miss a link that comes up this early, which no real join does."""
    b, detail = booted(cfg, "wifi-early", r, usb=False, env={"IT_WIFI_AUTOJOIN": "0.01"})
    try:
        text = open(b.serial, errors="replace").read()
        panic = re.search(r"panic\(.*", text)
        if panic:
            return r.set(False, panic.group(0)[:120])
        if not detail:
            return
        t0 = time.time()
        while 'ssid[ 8] = "qemu-ios"' not in text and time.time() - t0 < 60:
            time.sleep(2)
            text = open(b.serial, errors="replace").read()
        panic = re.search(r"panic\(.*", text)
        if panic:
            r.set(False, panic.group(0)[:120])
        elif 'ssid[ 8] = "qemu-ios"' in text:
            r.set(True, "joined qemu-ios 10 ms after the model armed the join, no panic")
        else:
            r.set(False, "lock screen, but never joined")
    finally:
        b.stop()


def check_audio(cfg, r):
    """a4-periph's WAV correlation (tests/ipad1/audio-check.py): boot sound, unlock, lock, unlock."""
    spec = importlib.util.spec_from_file_location("audio_check", os.path.join(HERE, "audio-check.py"))
    ac = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(ac)
    wav = os.path.join(cfg.out, "audio", "out.wav")
    b = Boot(cfg, "audio", usb=False, wav=wav)
    b.start()
    try:
        ok, detail = b.wait_lock_screen()
        if not ok:
            return r.set(False, detail)
        time.sleep(15)   # the boot sound comes ~35 s after SpringBoard starts
        expect = ac.capture_references(b.qmp, os.path.join(b.dir, "references"),
                                       cfg.product_version, getattr(cfg, "sound_reference_root", None))
        ac.play_sounds(b.qmp)
        b.qmp.cmd("quit")
        b.qemu.wait(timeout=30)                       # the WAV header is written at exit
    finally:
        b.stop()
    ok = ac.judge(wav, b.serial, expect=expect)
    r.set(ok, "%d sounds correlate >= 0.8 with the rootfs originals" % len(expect) if ok
          else "WAV correlation failed (see audio/ and the judge output above)")


SAFARI_BOOKMARKS = (57, 605)
# The Bookmarks popover spans panel x 88..488, y 425..768. Its drop shadow is CoreAnimation's A008 (8-bit
# alpha) surface; a bridge that refuses it samples the zero texture and paints a solid ~50% box over x 0..560,
# y 357..768 instead. Outside the box's reach of the soft shadow: white; hugging the popover edge: shaded.
SHADOW_CLEAR, SHADOW_EDGE = (530, 380), (492, 600)


def check_shadow(cfg, r):
    """(opt-in) Safari's Bookmarks popover casts a soft shadow, not a solid box (GL CA's A8 surfaces)."""
    b, detail = booted(cfg, "shadow", r, keyboard=True)
    try:
        if not detail:
            return
        b.drag(UNLOCK_FROM, UNLOCK_TO)
        time.sleep(2)
        b.tap(SAFARI_ICON)
        time.sleep(8)
        b.tap(SAFARI_BOOKMARKS)
        time.sleep(3)
        shot = b.shot("popover")
        w, h, pix = itqmp.read_ppm(shot)
        at = lambda xy: min(pix[(xy[1] * w + xy[0]) * 3:(xy[1] * w + xy[0]) * 3 + 3])
        clear, edge = at(SHADOW_CLEAR), at(SHADOW_EDGE)
        detail = "popover shadow: %d past its reach (>= 230), %d at the edge" % (clear, edge)
        if clear >= 230 and edge < clear:
            gl_clean(b, r, detail, [shot])
        else:
            r.set(False, detail)
    finally:
        b.stop()


PREFS = "/var/mobile/Library/Preferences/"
MC_SETTINGS = "/var/mobile/Library/ConfigurationProfiles/UserSettings.plist"
MC_NEVER = 0x7fffffff       # Settings' Never in ManagedConfiguration (4.x, 5.x)


def check_prefs(cfg, r):
    """it_prefs' once-only defaults on a device it has not run on before (a fresh prepare runs it in the
    seal boot): Brightness at maximum and Auto-Lock at Never, read back over the agent from where this
    firmware's Settings keeps them, and the marker that stops it applying them again."""
    b, detail = booted(cfg, "prefs", r)
    try:
        if not detail:
            return
        t0 = time.time()
        while time.time() - t0 < 180 and not itqmp.agent_alive(b.qmp):
            time.sleep(3)

        def plist(path):
            status, data = itqmp.agent(b.qmp, "get", path)
            return plistlib.loads(data) if status == 0 else {}
        while True:     # the job runs once the boot settles; a package installed this boot runs it now
            mine, sb, mc = plist(PREFS + "com.qemu.it-prefs.plist"), plist(PREFS + "com.apple.springboard.plist"), plist(MC_SETTINGS)
            if mine.get("DefaultsSet") or time.time() - t0 > 300:
                break
            time.sleep(5)
        bright = sb.get("SBBacklightLevel2", sb.get("SBBacklightLevel"))
        lock = ((mc.get("restrictedValue") or {}).get("maxInactivity") or {}).get("value")
        never = lock == MC_NEVER if cfg.major >= 4 else sb.get("SBAutoLockTime") == -1
        r.set(mine.get("DefaultsSet") is True and bright == 1.0 and never,
              "brightness %r, auto-lock %r (MC maxInactivity %r), marker %r" % (bright, sb.get("SBAutoLockTime"), lock, mine))
    finally:
        b.stop()


def check_app(cfg, r):
    """An IPA (--ipa, default the iPod harness) installs over installation_proxy (the device's AppSync takes the
    ad-hoc signature), installd lists it, the agent launches it and SpringBoard reports it frontmost on a lit
    screen."""
    ipa = getattr(cfg, "ipa", None) or HARNESS_IPA
    if not os.path.exists(ipa):
        return r.set(False, "no IPA at %s (contrib/it-harness/build.sh builds the harness)" % ipa)
    bundle = ipod.ipa_bundle_id(ipa)
    b, detail = booted(cfg, "app", r)
    try:
        if not detail:
            return
        p = b.run(["ideviceinstaller", "install", ipa], timeout=300)
        listed = bundle in b.run(["ideviceinstaller", "list"], timeout=120).stdout
        if p.returncode or not listed:
            return r.set(False, "install rc=%s, %s listed=%s: %s" % (p.returncode, bundle, listed,
                                                                      (p.stdout + p.stderr).strip()[-200:]))
        t0 = time.time()
        while not itqmp.agent_alive(b.qmp) and time.time() - t0 < 120:
            time.sleep(3)
        b.press("home")
        time.sleep(1.5)
        b.drag(UNLOCK_FROM, UNLOCK_TO)
        time.sleep(3)
        status, out = itqmp.agent(b.qmp, "launch", bundle, timeout=60)
        if status != 0:
            return r.set(False, "installed, but the agent's launch failed (%d): %s" % (status, out[-200:]))
        time.sleep(20)
        status, front = itqmp.agent(b.qmp, "frontmost")
        front = front.decode("utf-8", "replace").splitlines()[0] if status == 0 and front else ""
        ok, pic = b.picture("app")
        detail = "%s installed and listed; launched, frontmost %r, %s" % (bundle, front, pic)
        if front != bundle or not ok or getattr(cfg, "machine", "ipad1") not in ipad1_boot.PORTRAIT \
                or bundle != ipod.ipa_bundle_id(HARNESS_IPA):
            return r.set(front == bundle and ok, detail)
        # The harness's "GL: rotating triangle" (the first row, at its point size x2 on a Retina panel): its
        # EAGL frames go through the GLES front end to the host, and the bridge refuses nothing.
        log_path = os.path.join(b.dir, "qemu.log")
        before = open(log_path, errors="replace").read().count("[gles]")
        b.tap((310, 156))
        time.sleep(8)
        draws = open(log_path, errors="replace").read().count("[gles]") - before
        if draws <= 0:
            return r.set(False, detail + "; the GL scene sent nothing through the bridge")
        # The scene is a white triangle on cyan and magenta halves between the harness's toolbar and its frame counter.
        w, h, pix = itqmp.read_ppm(b.shot("app-gl-scene"))
        scene = {bytes(pix[(y * w + x) * 3:(y * w + x) * 3 + 3]) for y in range(140, 820, 8) for x in range(0, w, 8)}
        if len(scene) < 3:   # the triangle scene is four flat colours (black, cyan, magenta, white)
            return r.set(False, detail + "; the GL scene ran (%d bridge lines) but its view shows %d colour(s)"
                         % (draws, len(scene)))
        # The scene's own background is magenta, so judge the bridge by its refusal counters, not gl_clean's paint.
        rejects = b.qmp.cmd("qom-get", path="/machine", property="gles-rejects").strip()
        r.set(not rejects, detail + "; GL scene drawn (%d colours, %d bridge log lines); bridge refused %s"
              % (len(scene), draws, rejects.replace("\n", ", ") or "nothing"))
    finally:
        b.stop()


def check_nocharge(cfg, r):
    """usb-charger=off over the usbmuxd bridge: a port that supplies no charge current. The bridge's charge
    request never reaches the guest, so the iPad stays at 500 mA: connected, not charge-capable, not
    charging (the status bar's "Not Charging", the lock screen's wallpaper), and AFC still moves files."""
    b = Boot(cfg, "nocharge", machine="usb-charger=off")
    try:
        b.start()
        ok, detail = b.wait_lock_screen()
        if not ok or not b.wait_mux():
            r.set(False, detail if not ok else "lock screen but usbmux never attached")
            return
        info = {}
        t0 = time.time()
        while time.time() - t0 < 120 and info.get("ExternalConnected") is None:
            info = battery_info(b)
            time.sleep(5)
        bad = afc_roundtrip(b, [70001], "nocharge")
        want = {"ExternalConnected": "true", "ExternalChargeCapable": "false", "BatteryIsCharging": "false"}
        r.set(all(info.get(k) == v for k, v in want.items()) and not bad,
              "%s; AFC %s" % (", ".join("%s %s" % (k, info.get(k)) for k in want), "ok" if not bad else "mismatch %r" % bad))
    finally:
        b.stop()


CHECKS = {"boot": check_boot, "gles": check_gles, "shadow": check_shadow, "usbmux": check_usbmux, "afc": check_afc,
          "persist": check_persist, "net": check_net, "net-usb": check_net_usb, "wifi": check_wifi,
          "wifi-early": check_wifi_early, "audio": check_audio, "prefs": check_prefs, "nocharge": check_nocharge,
          "app": check_app}


def device_args(a):
    """--nand defaults to the --device's; product_version from the store's device.lock.json.
    Boot images (iBoot, NOR, catalog keys, die-id, or an explicit --kboot) come from ipad1_boot."""
    a.nand = a.nand or os.path.join(a.device, "nand")
    lock = os.path.join(os.path.dirname(os.path.abspath(a.nand)), "device.lock.json")
    lockd = json.load(open(lock)) if os.path.exists(lock) else {}
    a.product_version = getattr(a, "product_version", None) or lockd.get("product_version", "3.2.2")
    a.build = lockd.get("build")  # visual references require evidence, never an assumed build
    a.udid = (lockd.get("identity") or {}).get("udid")
    a.major = int(a.product_version.split(".")[0])
    a.gl_test = bool(lockd.get("gl_test"))      # it_gltest's scene sits over SpringBoard's screens
    a.activated = bool((lockd.get("inputs") or {}).get("activation") or (lockd.get("inputs") or {}).get("activation_hook"))
    a.package_seed = (lockd.get("guest_package") or {}).get("seed")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--require-inputs", action="store_true", help="fail when a selected check is skipped")
    ap.add_argument("--checks", default=",".join(DEFAULT_CHECKS))
    ap.add_argument("--nand", help="override the selected device NAND")
    ipad1_boot.add_arguments(ap)
    ap.add_argument("--qemu", default=os.path.join(ROOT, "build/qemu-system-arm"))
    ap.add_argument("--usbmuxd", default=USBMUXD)
    ap.add_argument("--ipa", help="app check: the IPA to install and launch (default the iPod harness)")
    ap.add_argument("--boot-timeout", type=int, default=600, help="hard cap per QEMU, seconds")
    ap.add_argument("--out", default=None)
    ap.add_argument("--product-version", help="usbmux's expected ProductVersion (default: NAND/../device.lock.json, else 3.2.2)")
    ap.add_argument("--sound-reference-root", help="explicit matching extracted rootfs when the guest has no agent")
    ap.add_argument("--guest-package", metavar="DIR", help="the machine's guest-package offer directory "
                    "(contrib/guest-package/mkpkg.py offer); boot then also wants it_boot's report")
    a = ap.parse_args()
    if not any(a.checks.split(",")): ap.error("no checks selected")
    itqmp.W, itqmp.H = ipad1_boot.MACHINES[a.machine]
    if a.machine in ipad1_boot.PORTRAIT:
        # A plugged-in iPod's lock screen is the charging battery on black, not the wallpaper: ~30% lit.
        global LIT_MIN_FRACTION, UNLOCK_FROM, UNLOCK_TO
        LIT_MIN_FRACTION = 0.2
        UNLOCK_FROM, UNLOCK_TO = (116, 862), (600, 862)   # portrait panel: the slider runs along the bottom
    device_args(a)
    import ffmpeg_guard                     # imgtools; stock FFmpeg breaks iPod H.264
    why = ffmpeg_guard.check(a.qemu)
    if why:
        sys.exit(why)
    ipod.START = time.time()
    a.out = a.out or tempfile.mkdtemp(prefix="ipad1regress-")
    os.makedirs(a.out, exist_ok=True)
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
    with concurrent.futures.ThreadPoolExecutor(max(1, len(runnable))) as pool:   # all checks at once
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
    failed = [c for c in selected if results[c].ok is False or (a.require_inputs and results[c].skipped)]
    print("%d check(s) failed; artifacts in %s; %.1f min" % (len(failed), a.out, (time.time() - ipod.START) / 60))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
