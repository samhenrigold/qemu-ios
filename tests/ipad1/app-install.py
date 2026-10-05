#!/usr/bin/env python3
"""Install gate for A4 devices: an IPA through installation_proxy (AppSync), launched from the home screen, and
(the Harness) its GLES row drawing through the GL bridge.

    tests/ipad1/app-install.py --machine iPod-Touch-4G --device DEV [--kboot K --nor NOR] [--ipa IPA] --out OUT

Boot (regress.py's Boot, usbmuxd bridge) -> ideviceinstaller install + list -> unlock -> springboardservices puts
the icon on page 1 -> tap it -> the app's process must show in syslog and the frame must leave the home screen ->
(--gl-tap, default the Harness's GLES row) tap, then the frame must be mostly the fixture's cyan/magenta with no
bridge refusals -> guest power-off. Screens land in OUT/install/*.png. Exit status 0 only if every step passed.
"""
import argparse, importlib.util, os, plistlib, re, shutil, subprocess, sys, time, zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
HARNESS = os.path.join(ROOT, "contrib/it-harness/build/Harness.ipa")


def load(name, file):
    spec = importlib.util.spec_from_file_location(name, os.path.join(HERE, file))
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


rg = load("ipad1_regress", "regress.py")
ac = load("ipad1_app_compat", "app-compat.py")

# Home-screen icon centres, (row, col) -> screen point, per machine (measured on the stock layouts).
GRID = {"ipad1": ac.GRID,
        "iPod-Touch-4G": lambda r, c: (91 + 152 * c, 125 + 176 * r),   # 4x4 grid on the 640x960 portrait panel
        "n18": lambda r, c: (38 + 79 * c, 52 + 88 * r),                # the same grid at 320x480
        "n88": lambda r, c: (38 + 79 * c, 52 + 88 * r)}
GL_TAP = {"ipad1": (0.343, 0.5), "iPod-Touch-4G": (0.5, 0.165), "n18": (0.5, 0.165), "n88": (0.5, 0.165)}         # the Harness's GLES 1.1 row


def bundle_of(ipa):
    with zipfile.ZipFile(ipa) as z:
        name = next(n for n in z.namelist() if re.match(r"Payload/[^/]+\.app/Info\.plist$", n))
        info = plistlib.loads(z.read(name))
    return info["CFBundleIdentifier"], info.get("CFBundleExecutable", "")


def png(b, name):
    ppm = b.shot(name)
    out = os.path.join(b.dir, name + ".png")
    subprocess.run(["sips", "-s", "format", "png", ppm, "--out", out], capture_output=True)
    return ppm


def fixture_fraction(ppm):
    """Fraction of sampled pixels that are the Harness GL fixture's cyan or magenta background."""
    w, h, pix = rg.itqmp.read_ppm(ppm)
    n = hit = 0
    for i in range(0, len(pix) - 2, 3 * 101):
        r, g, bl = pix[i:i + 3]
        n += 1
        hit += (r < 80 and g > 150 and bl > 150) or (r > 150 and g < 80 and bl > 150)
    return hit / max(n, 1)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    rg.ipad1_boot.add_arguments(ap)
    ap.add_argument("--nand")
    ap.add_argument("--ipa", default=HARNESS)
    ap.add_argument("--gl-tap", help="normalized X,Y to tap after launch ('' to skip; default the Harness's GLES row)")
    ap.add_argument("--qemu", default=os.path.join(ROOT, "build/qemu-system-arm"))
    ap.add_argument("--usbmuxd", default=rg.USBMUXD)
    ap.add_argument("--product-version")
    ap.add_argument("--boot-timeout", type=int, default=560)
    ap.add_argument("--overlay", help="start from a copy of this NAND overlay (e.g. a 5.x device past Setup)")
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    rg.itqmp.W, rg.itqmp.H = rg.ipad1_boot.MACHINES[a.machine]
    if a.machine in rg.ipad1_boot.PORTRAIT:
        rg.LIT_MIN_FRACTION = 0.2
        rg.UNLOCK_FROM, rg.UNLOCK_TO = rg.portrait_unlock()
    rg.device_args(a)
    rg.ipod.START = time.time()
    bundle, exe = bundle_of(a.ipa)
    gl = a.gl_tap if a.gl_tap is not None else (",".join(map(str, GL_TAP[a.machine])) if a.ipa == HARNESS else "")
    steps = []

    def step(name, ok, detail):
        steps.append((name, ok, detail))
        rg.log("  %-8s %s  %s" % (name, "PASS" if ok else "FAIL", detail))
        return ok

    overlay = None
    if a.overlay:
        overlay = os.path.join(a.out, "install", "overlay")
        shutil.rmtree(overlay, ignore_errors=True)
        os.makedirs(os.path.dirname(overlay), exist_ok=True)
        subprocess.run(["cp", "-cR", a.overlay, overlay], check=True)   # a clone on APFS; the original stays as it was
    b = rg.Boot(a, "install", usb=True, overlay=overlay)
    try:
        b.start()
        if not step("mux", b.wait_mux(), "lockdown answers ProductVersion %s" % a.product_version):
            return 1
        syslog = os.path.join(b.dir, "syslog.log")
        b.procs.spawn(["idevicesyslog"], syslog, env=b.env())
        ins = b.run(["ideviceinstaller", "install", a.ipa], timeout=240)
        listed = bundle in b.run(["ideviceinstaller", "list"], timeout=90).stdout
        out = (ins.stdout + ins.stderr).strip().splitlines()
        if not step("install", listed, "%s listed; installer: %s" % (bundle, out[-1] if out else "")):
            return 1
        ok, det = b.wait_lock_screen(timeout=300)
        if not step("lock", ok, det):
            return 1
        # The S5L8920 boards power the digitizer down on the lock screen (DisablePowerForUILock): Home first.
        wake = (lambda: (b.press("home"), time.sleep(1))) if a.machine in ("n18", "n88") else (lambda: None)
        wake()
        b.drag(rg.UNLOCK_FROM, rg.UNLOCK_TO)
        time.sleep(4)
        slot = None
        for _ in range(6):   # springboardservices answers once SpringBoard is past the lock screen
            slot = ac.pin_to_page1(b, bundle) or ac.icon_slot(b, bundle)
            if slot:
                break
            wake()
            b.drag(rg.UNLOCK_FROM, rg.UNLOCK_TO)
            time.sleep(5)
        if not step("icon", bool(slot) and slot[0] == 1, "springboardservices slot %s" % (slot,)):
            return 1
        if a.machine in rg.ipad1_boot.PORTRAIT:
            # Dismiss the install's Edit-Home-Screen help sheet (4.x); with none up, x 0.5 falls between icon columns.
            b.tap((0.5 * rg.itqmp.W, 0.69 * rg.itqmp.H))
            time.sleep(2)
        home = png(b, "home")
        mark = os.path.getsize(syslog) if os.path.exists(syslog) else 0
        b.tap(GRID[a.machine](slot[1], slot[2]))
        time.sleep(12)
        app = png(b, "launched")
        log = open(syslog, errors="replace").read()[mark:] if os.path.exists(syslog) else ""
        # "Harness[75]", or launchd's "UIKitApplication:com.qemuios.harness[0x6a01][75]" (4.x)
        started = bool(re.search(r"(%s|%s)(\[0x[0-9a-f]+\])?\[\d+\]" % (re.escape(exe), re.escape(bundle)), log)) \
            or rg.frontmost(b) == bundle    # 5.x logs no launch line; the guest agent names the frontmost app
        changed = ac._framediff(ac._sample(rg, home), ac._sample(rg, app))
        if not step("launch", started and changed, "process in syslog %s, frame changed %s" % (started, changed)):
            return 1
        if gl:
            x, y = map(float, gl.split(","))
            b.tap((x * rg.itqmp.W, y * rg.itqmp.H))
            time.sleep(8)
            frac = fixture_fraction(png(b, "gl"))
            rej = rg.itqmp.gles_rejects(b.qmp)
            log = open(syslog, errors="replace").read()
            said = re.findall(r"\[Harness\] ((?:PASS|FAIL)[^\n]*GLES[^\n]*)", log)
            ok = frac > 0.3 and not rej and (a.ipa != HARNESS or (said and not any(s.startswith("FAIL") for s in said)))
            step("gl", ok, "fixture colours %.0f%% of the frame, bridge refusals %s; %s" % (
                frac * 100, rej or "none", "; ".join(said) or "no GLES report"))
        ok = b.powerdown()
        step("shutdown", bool(ok), "guest power-off %s" % ok)
    finally:
        b.stop()
        print("=" * 60)
        for name, ok, detail in steps:
            print("%s  %-8s %s" % ("PASS" if ok else "FAIL", name, detail))
    return 0 if steps and all(ok for _, ok, _ in steps) else 1


if __name__ == "__main__":
    sys.exit(main())
