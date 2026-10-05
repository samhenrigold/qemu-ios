#!/usr/bin/env python3
"""Install gate for A4 devices: an IPA through installation_proxy (AppSync), launched from the home screen, and
(the Harness) its GLES row drawing through the GL bridge.

    tests/ipad1/app-install.py --machine iPod-Touch-4G --device DEV [--kboot K --nor NOR] [--ipa IPA] --out OUT

Boot (regress.py's Boot, usbmuxd bridge) -> ideviceinstaller install + list -> unlock -> springboardservices puts
the icon on page 1 -> tap it -> the app's process must show in syslog and the frame must leave the home screen ->
(--gl-tap, default the Harness's GLES row) tap, then the frame must be mostly the fixture's cyan/magenta with no
bridge refusals -> guest power-off. Screens land in OUT/install/*.png. Exit status 0 only if every step passed.
"""
import argparse, importlib.util, os, plistlib, re, subprocess, sys, time, zipfile

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
        "iPhone-4": lambda r, c: (91 + 152 * c, 125 + 176 * r),
        "n18": lambda r, c: (38 + 79 * c, 52 + 88 * r),                # the same grid at 320x480
        "n88": lambda r, c: (38 + 79 * c, 52 + 88 * r)}
GL_TAP = {"ipad1": (0.343, 0.5)}                                       # the Harness's GLES 1.1 row
GL_TAP.update((m, (0.5, 0.165)) for m in rg.ipad1_boot.PORTRAIT)


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


def ocr_upright(ppm):
    """{text: (x, y)} on an upright portrait screendump (the iPod's panel): regress.py's ocr turns the panel a
    quarter for the iPad, so the frame goes in turned the other way and the result comes back unturned."""
    w, h, pix = rg.itqmp.read_ppm(ppm)
    out = bytearray(w * h * 3)
    for y in range(h):                       # turned (X = y, Y = w - 1 - x): its left edge is the upright top
        row = pix[y * w * 3:(y + 1) * w * 3]
        for x in range(w):
            o = ((w - 1 - x) * h + y) * 3
            out[o:o + 3] = row[x * 3:x * 3 + 3]
    turned = ppm + ".turned.ppm"
    with open(turned, "wb") as f:
        f.write(b"P6\n%d %d\n255\n" % (h, w) + bytes(out))
    found = {}
    try:
        rg.ocr(turned)                       # builds the OCR tool once
        lines = subprocess.run([rg.OCR_BIN, turned], capture_output=True, text=True, check=True).stdout.splitlines()
    except subprocess.CalledProcessError:    # Vision crashes on some frames (4.x home screens): no text read
        return found
    for x0, y0, x1, y1, text in sorted((l.split(" ", 4) for l in lines), key=lambda l: int(l[1]), reverse=True):
        found[text.strip()] = ((int(x0) + int(x1)) // 2, (int(y0) + int(y1)) // 2)
    return found


# iOS 5's Setup Assistant on the iPod (tests/ipad1/regress.py walks the iPad's): each page is answered by the
# first label of PICKS it shows, then its Next (the language page's is an arrow); a button labelled exactly as one\n# of ALERT_YES (an alert's, or Terms' Agree) first.
PICKS = ("Start Using iPod touch", "Start Using iPod", "Start Using iPhone", "Set Up as New iPod touch", "Set Up as New iPod",
         "Set Up as New iPhone", "Disable Location Services", "Skip This Step", "Agree",
         "Don't Send", "Australia", "United States")
ALERT_YES = ("OK", "Skip", "Agree", "Continue")
NEXT_ARROW = (587, 84)                       # at 640x960; walk_setup scales it to the panel


def walk_setup(b, step):
    """From Setup's first page to the home screen: (ok, detail)."""
    pages = []
    for n in range(40):
        time.sleep(3)
        found = ocr_upright(b.shot("setup-%02d" % n))
        if "Safari" in found and "English" not in found:
            return True, "Setup walked: " + ", ".join(pages)
        alert = next((t for t in ALERT_YES if t in found), None)        # a button labelled exactly so
        if alert:
            b.tap(found[alert])
            pages.append("(%s)" % alert)
            continue
        pick = next((t for t in PICKS if t in found), None)
        if pick:
            # a label tapped again and again: nudge the tap (the digitizer's edges are uncalibrated, README debt 7)
            again = pages.count(pick)
            x, y = found[pick]
            b.tap((x, y + (0, -14, 14, -24, 24)[again % 5]))
            pages.append(pick)
            if pick.startswith("Start Using"):
                continue
            time.sleep(1.5)
        arrow = (NEXT_ARROW[0] * rg.itqmp.W // 640, NEXT_ARROW[1] * rg.itqmp.H // 960)
        nxt = found.get("Next", arrow if "English" in found else None)
        if nxt:
            b.tap(nxt)
            if not pick:
                pages.append(next((t for t, (x, y) in found.items() if y < 130 and t != "Next"), "?"))
    return False, "Setup still up after 40 pages: " + ", ".join(pages)


def harness_results(b, bundle):
    """The Harness's Documents/results.log (house_arrest), every line it has reported; "" if unreadable."""
    out = os.path.join(b.dir, "results.log")
    if os.path.exists(out):
        os.unlink(out)
    b.run(["afcclient", "--container", bundle, "get", "Documents/results.log", out], timeout=60)
    return open(out, errors="replace").read() if os.path.exists(out) else ""


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
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    rg.itqmp.W, rg.itqmp.H = rg.ipad1_boot.MACHINES[a.machine]
    portrait = a.machine in rg.ipad1_boot.PORTRAIT
    if portrait:
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

    b = rg.Boot(a, "install", usb=True)
    try:
        b.start()
        if not step("mux", b.wait_mux(), "lockdown answers ProductVersion %s" % a.product_version):
            return 1
        ins = b.run(["ideviceinstaller", "install", a.ipa], timeout=240)
        listed = bundle in b.run(["ideviceinstaller", "list"], timeout=90).stdout
        out = (ins.stdout + ins.stderr).strip().splitlines()
        if not step("install", listed, "%s listed; installer: %s" % (bundle, out[-1] if out else "")):
            return 1
        ok, det = b.wait_lock_screen(timeout=300)
        if not step("lock", ok, det):
            return 1

        def unlock():
            b.press("home")  # the panel may have slept while the install ran; the S5L8920 boards power the digitizer
            # down on the lock screen (DisablePowerForUILock)
            time.sleep(1.5)
            b.drag(rg.UNLOCK_FROM, rg.UNLOCK_TO)

        fresh = False
        for _ in range(3 if portrait else 1):
            unlock()
            time.sleep(4)
            fresh = portrait and "English" in ocr_upright(b.shot("opened"))   # a fresh 5.x
            status, out = rg.itqmp.agent(b.qmp, "lockstatus") if rg.itqmp.agent_alive(b.qmp) else (1, b"")
            if fresh or status or b"locked=0" in out:
                break                                # Setup's language page, past the lock, or no agent to ask
        if fresh:
            if not step("setup", *walk_setup(b, step)):
                return 1
        slot = None
        for _ in range(6):   # springboardservices answers once SpringBoard is past the lock screen
            slot = ac.pin_to_page1(b, bundle) or ac.icon_slot(b, bundle)
            if slot:
                break
            unlock()
            time.sleep(5)
        if not step("icon", bool(slot) and slot[0] == 1, "springboardservices slot %s" % (slot,)):
            return 1
        if portrait:
            # Dismiss the install's Edit-Home-Screen help sheet (4.x); with none up, x 0.5 falls between icon columns.
            b.tap((0.5 * rg.itqmp.W, 0.69 * rg.itqmp.H))
            time.sleep(2)
        # The relay starts here, not at install: Setup's end re-enumerates the USB device and idevicesyslog exits.
        syslog = os.path.join(b.dir, "syslog.log")
        b.procs.spawn(["idevicesyslog"], syslog, env=b.env())
        time.sleep(3)
        home = png(b, "home")
        mark = os.path.getsize(syslog) if os.path.exists(syslog) else 0
        b.tap(GRID[a.machine](slot[1], slot[2]))
        time.sleep(12)
        app = png(b, "launched")
        log = open(syslog, errors="replace").read()[mark:] if os.path.exists(syslog) else ""
        # "Harness[75]", or launchd's "UIKitApplication:com.qemuios.harness[0x6a01][75]" (4.x)
        started = bool(re.search(r"(%s|%s)(\[0x[0-9a-f]+\])?\[\d+\]" % (re.escape(exe), re.escape(bundle)), log))
        if not started and rg.itqmp.agent_alive(b.qmp):   # the syslog relay can drop with the USB link; ask SpringBoard
            status, front = rg.itqmp.agent(b.qmp, "frontmost")
            started = status == 0 and front.split(b"\n")[0] == bundle.encode()
        if not started and a.ipa == HARNESS:     # 5.1+ launchd no longer sends an app's stderr to syslog
            started = "Harness 1.0 | iOS" in harness_results(b, bundle) or \
                any(t.startswith("Harness 1.0") for t in ocr_upright(app))
        changed = ac._framediff(ac._sample(rg, home), ac._sample(rg, app))
        if not step("launch", started and changed, "process seen %s, frame changed %s" % (started, changed)):
            return 1
        if gl:
            x, y = map(float, gl.split(","))
            b.tap((x * rg.itqmp.W, y * rg.itqmp.H))
            time.sleep(8)
            frac = fixture_fraction(png(b, "gl"))
            # a transfer CA queued and never notified (it tore the layer down first) is not a frame the bridge refused
            rej = {k: v for k, v in rg.itqmp.gles_rejects(b.qmp).items() if k != "shim:scaler:token-dropped"}
            said = re.findall(r"^\S+ ((?:PASS|FAIL)[^\n]*GLES[^\n]*)", harness_results(b, bundle), re.M)   # "<time> <line>"
            if not said and os.path.exists(syslog):  # a Harness without results.log reports through syslog
                said = re.findall(r"\[Harness\] ((?:PASS|FAIL)[^\n]*GLES[^\n]*)", open(syslog, errors="replace").read())
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
