#!/usr/bin/env python3
"""iPad 1 / 3.2.2 app-compatibility pass over a folder of IPAs.

    tests/ipad1/app-compat.py inventory [DIR] [--md docs/ipad1/app-compat.md]
    tests/ipad1/app-compat.py run [DIR] [--only SUBSTR] [--limit N] [--nand STORE] [--out DIR]
    tests/ipad1/app-compat.py --selfcheck

inventory: bundle id, name, MinimumOSVersion, UIDeviceFamily, arch and encryption state
for every IPA under DIR (default ~/Downloads/ios3, recursive), plus the candidates that
can run here (decrypted, MinimumOS <= 3.2). Read from the zip in place: no extraction.

run: for each candidate, boot a fresh overlay of golden-appsync, install it over the
usbmuxd bridge, unlock, tap the new icon to launch, wait, screenshot, pull crash logs,
and record PASS / CRASH / NO-LAUNCH plus any "[glishim] unimplemented" GL lines. Reuses
tests/ipad1/regress.py's Boot. One QEMU per app; nothing runs in the background.
"""
import argparse, importlib.util, os, plistlib, re, struct, sys, time, zipfile

DEFAULT_DIR = os.path.expanduser("~/Downloads/ios3")
MAX_OS = (3, 2)
CPU = {(12, 6): "armv6", (12, 9): "armv7", (12, 0): "arm", (12, 11): "armv7s"}
FAMILY = {1: "iPhone", 2: "iPad"}


def slices(b):
    """[(arch, encrypted_bool)] for a thin or fat 32-bit Mach-O blob."""
    if len(b) < 8:
        return []
    magic = struct.unpack_from(">I", b)[0]
    if magic == 0xCAFEBABE:                       # fat, big-endian table
        out = []
        for i in range(struct.unpack_from(">I", b, 4)[0]):
            ct, st, off, size, _ = struct.unpack_from(">iiIII", b, 8 + 20 * i)
            out += _thin(b[off:off + size])
        return out
    return _thin(b)


def _thin(b):
    if len(b) < 28:
        return []
    magic = struct.unpack_from("<I", b)[0]
    le = magic in (0xFEEDFACE, 0xFEEDFACF)
    if not le:
        return []
    ct, st, _, ncmds = struct.unpack_from("<iiII", b, 4)
    arch = CPU.get((ct, st & 0xFF), "cpu%d/%d" % (ct, st & 0xFF))
    enc, off = False, 28 if magic == 0xFEEDFACE else 32
    for _ in range(ncmds):
        cmd, sz = struct.unpack_from("<II", b, off)
        if cmd == 0x21:                           # LC_ENCRYPTION_INFO: cryptid at +12
            enc = struct.unpack_from("<I", b, off + 16)[0] != 0
        off += sz
    return [(arch, enc)]


def parse_version(v):
    """'3.1.3' -> (3,1,3); best-effort, missing -> (99,)."""
    if not v:
        return (99,)
    parts = []
    for p in str(v).split("."):
        try:
            parts.append(int(p))
        except ValueError:
            break
    return tuple(parts) or (99,)


def inspect(path):
    """Read one IPA: {bundle,name,minos,family,archs,encrypted,error}."""
    r = {"file": os.path.basename(path), "bundle": "", "name": "", "minos": "",
         "family": "", "archs": [], "encrypted": None, "error": ""}
    try:
        with zipfile.ZipFile(path) as z:
            names = z.namelist()
            # .../Payload/Foo.app/Info.plist, the shallowest one
            infos = sorted((n for n in names if n.endswith(".app/Info.plist")
                            and n.count("/") == 2), key=len)
            if not infos:
                infos = sorted(n for n in names if n.endswith(".app/Info.plist"))
            if not infos:
                r["error"] = "no Info.plist"
                return r
            info = infos[0]
            appdir = info[:-len("Info.plist")]
            d = plistlib.loads(z.read(info))
            r["bundle"] = d.get("CFBundleIdentifier", "")
            r["name"] = d.get("CFBundleDisplayName") or d.get("CFBundleName", "")
            r["minos"] = d.get("MinimumOSVersion") or d.get("LSMinimumSystemVersion", "")
            fam = d.get("UIDeviceFamily")
            if isinstance(fam, list):
                r["family"] = "+".join(FAMILY.get(int(f), str(f)) for f in fam)
            elif fam is not None:
                r["family"] = FAMILY.get(int(fam), str(fam))
            else:
                r["family"] = "iPhone"            # pre-3.2 default (no key)
            exe = d.get("CFBundleExecutable")
            if exe:
                try:
                    sl = slices(z.read(appdir + exe))
                    r["archs"] = [a for a, _ in sl]
                    r["encrypted"] = any(e for _, e in sl) if sl else None
                except KeyError:
                    r["error"] = "exe not in zip"
    except Exception as e:                         # noqa: BLE001 - report, never crash the sweep
        r["error"] = "%s: %s" % (type(e).__name__, e)
    return r


def is_candidate(r):
    """Decrypted and MinimumOS <= 3.2 and has an arm slice."""
    return (r["encrypted"] is False and not r["error"]
            and parse_version(r["minos"]) <= MAX_OS
            and any(a.startswith("arm") for a in r["archs"]))


def scan(root):
    rows = []
    for dirpath, _, files in os.walk(root):
        for f in sorted(files):
            if f.lower().endswith(".ipa"):
                rows.append(inspect(os.path.join(dirpath, f)))
    rows.sort(key=lambda r: r["file"].lower())
    return rows


def to_md(rows, root):
    cand = [r for r in rows if is_candidate(r)]
    enc = [r for r in rows if r["encrypted"]]
    err = [r for r in rows if r["error"]]
    out = ["# iPad 1 / 3.2.2 app-compatibility inventory", "",
           "Source: `%s` (%d IPAs). Generated by `tests/ipad1/app-compat.py inventory`." % (root, len(rows)),
           "",
           "- **%d candidates** (decrypted, MinimumOS <= 3.2, arm slice) — installable/launchable here." % len(cand),
           "- %d still FairPlay-encrypted (out of scope until decrypted)." % len(enc),
           "- %d unreadable / no Info.plist." % len(err),
           "",
           "## Candidates", "",
           "| app | bundle id | min OS | family | arch |", "|---|---|---|---|---|"]
    for r in cand:
        out.append("| %s | `%s` | %s | %s | %s |" % (
            r["name"] or r["file"], r["bundle"], r["minos"] or "-", r["family"], ",".join(r["archs"]) or "-"))
    out += ["", "## All IPAs", "",
            "| file | name | bundle id | min OS | family | arch | enc | note |",
            "|---|---|---|---|---|---|---|---|"]
    for r in rows:
        enc_s = "-" if r["encrypted"] is None else ("yes" if r["encrypted"] else "no")
        out.append("| %s | %s | `%s` | %s | %s | %s | %s | %s |" % (
            r["file"], r["name"], r["bundle"], r["minos"] or "-", r["family"] or "-",
            ",".join(r["archs"]) or "-", enc_s, r["error"] or ""))
    return "\n".join(out) + "\n"


def selfcheck():
    assert parse_version("3.1.3") == (3, 1, 3)
    assert parse_version("3.2") == (3, 2) and parse_version("3.2") <= MAX_OS
    assert parse_version("4.0") > MAX_OS and parse_version("") > MAX_OS
    # a minimal thin armv6 Mach-O with an unencrypted LC_ENCRYPTION_INFO
    hdr = struct.pack("<IiiIIII", 0xFEEDFACE, 12, 6, 2, 1, 20 + 20, 0)
    lc = struct.pack("<IIIIII", 0x21, 20, 0, 0, 0, 0)   # cmd,size,cryptoff,cryptsize,cryptid=0,pad
    assert slices(hdr + lc) == [("armv6", False)]
    lc_enc = struct.pack("<IIIIII", 0x21, 20, 0, 0, 1, 0)
    assert slices(hdr + lc_enc) == [("armv6", True)]
    assert is_candidate({"encrypted": False, "error": "", "minos": "3.1", "archs": ["armv6"]})
    assert not is_candidate({"encrypted": True, "error": "", "minos": "3.1", "archs": ["armv6"]})
    assert not is_candidate({"encrypted": False, "error": "", "minos": "4.0", "archs": ["armv7"]})
    print("selfcheck OK")


# ---- launch pass -----------------------------------------------------------
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
FILES = os.path.expanduser("~/Developer/qemu-ios-files/ipad1")
# Upright portrait UI (interface 1) on the 1024x768 landscape scanout: portrait top is the panel's left edge.
DISMISS_EDIT = (615, 297)        # "Dismiss" on the install's Edit-Home-Screen help sheet
NEXT_PAGE = ((511, 87), (511, 617))      # swipe to the next home page (portrait right-to-left)
LAUNCH_WAIT = 9


def GRID(row, col):
    """(row,col) in the 4x5 portrait home grid -> panel coords on the 1024x768 scanout.
    Calibrated to the stock 7B500 layout via sbservices: slot (0,0)=(128,652)."""
    return (128 + 165 * row, 652 - 177 * col)


def _sample(rg, ppm, step=997):
    """Flat list of sampled pixel bytes from a ppm, or [] if unreadable."""
    try:
        _, _, pix = rg.itqmp.read_ppm(ppm)
        return list(pix[::step])
    except Exception:
        return []


def snap(b, rg, name, step=997):
    """(normalized_nonzero, backlight-normalized sample). Uses itqmp.shot so a dim/asleep
    panel is rescaled — regress Boot.picture() reads raw pixels and sees a lit screen as 0%."""
    png = os.path.join(b.dir, name + ".png")
    try:
        _, hi, nz = rg.itqmp.shot(b.qmp, png)
    except Exception:
        return (0.0, [])
    try:
        _, _, pix = rg.itqmp.read_ppm(png + ".ppm")
        s = pix[::step]
        m = max(s) or 1
        return (nz, [min(255, v * 255 // m) for v in s])   # per-frame normalized -> backlight-independent
    except Exception:
        return (nz, [])


def _framediff(a, b, thresh=8):
    """True if two sampled frames differ meaningfully (mean abs byte diff > thresh)."""
    if not a or not b or len(a) != len(b):
        return True
    d = sum(abs(x - y) for x, y in zip(a, b)) / len(a)
    return d > thresh


def _lit(sample):
    return sum(1 for v in sample if v > 12) / max(1, len(sample))


def wait_stable(b, rg, tag, deadline, want_lit=False):
    """Poll (backlight-normalized) frames until two consecutive settle, event-driven so it
    doesn't depend on host speed. If want_lit, wake the panel (home) and require a lit frame.
    Returns the settled sample."""
    nz, prev = snap(b, rg, "w-" + tag)
    while time.time() < deadline:
        if want_lit and nz <= 0.30:
            rg.itqmp.button(b.qmp, "home")     # wake a slept/booting panel
        nz, cur = snap(b, rg, "w-" + tag)
        if not _framediff(prev, cur) and (not want_lit or nz > 0.30):
            return cur
        prev = cur
    return prev


def _load_regress():
    spec = importlib.util.spec_from_file_location("ipad_regress", os.path.join(HERE, "regress.py"))
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    m.ipod.START = time.time()   # ipod.log() reads this; normally set in ipod.main()
    return m


SBICONS = os.path.join(ROOT, "build/ipad1-tools/sbicons")


def icon_slot(b, bundle):
    """(home_page, row, col) of bundle from lockdown springboardservices, or None.

    sbservices icon state is [dock, page1, page2, ...]; each page is rows of 4 cells
    (a cell is a dict with displayIdentifier/bundleIdentifier, or False for empty).
    home_page is 1-based (page1 = first shown home screen, needs 0 swipes)."""
    if not os.path.exists(SBICONS):
        return None
    p = b.run([SBICONS], timeout=45)
    if p.returncode != 0 or not p.stdout:
        return None
    try:
        state = plistlib.loads(p.stdout.encode())
    except Exception:
        return None
    for page_idx, page in enumerate(state[1:], start=1):     # skip [0] = dock
        for row_idx, row in enumerate(page):
            cells = row if isinstance(row, list) else [row]
            for col_idx, cell in enumerate(cells):
                if isinstance(cell, dict) and bundle in (cell.get("displayIdentifier"), cell.get("bundleIdentifier")):
                    return (page_idx, row_idx, col_idx)
    return None


def _syslog_has(path, needles, since=0):
    """(matched_bool, new_size): scan the syslog from byte offset `since` for any needle."""
    try:
        with open(path, errors="replace") as f:
            f.seek(since)
            txt = f.read()
        low = txt.lower()
        return (any(n.lower() in low for n in needles if n), since + len(txt.encode("utf-8", "replace")))
    except OSError:
        return (False, since)


def syslog_launch(b, rg, cfg, r, res, syslog, end):
    """Launch verdict from idevicesyslog alone (no screendumps): navigate blind to the
    app's icon (sbservices gives page/slot), tap, and decide from the app's own syslog
    lines and crash reports. Screendumps are black under GL-CA, so pixels aren't used."""
    bundle = r["bundle"]
    appname = (r["name"] or "").replace(" ", "")
    launch_needles = [bundle, bundle.split(".")[-1], appname]
    # 1. SpringBoard readiness gate WITHOUT pixels: springboardservices only answers once
    # SpringBoard is up, and it returns the icon layout, so poll it until the new app appears.
    slot = None
    while time.time() < end:
        slot = icon_slot(b, bundle)
        if slot:
            break
        time.sleep(8)
    if not slot:
        res["verdict"], res["note"] = "NO-BOOT", "SpringBoard/springboardservices never listed the app"
        return res
    page, row, col = slot
    res["note"] = "slot p%d r%d c%d" % slot
    time.sleep(5)
    # 3. blind navigation (no frame feedback): unlock, dismiss the install help sheet, page
    # over, tap. Timed; verdict comes from syslog so a missed tap just reads as NO-LAUNCH.
    b.drag(rg.UNLOCK_FROM, rg.UNLOCK_TO); time.sleep(2)
    b.drag(rg.UNLOCK_FROM, rg.UNLOCK_TO); time.sleep(2)   # twice, in case the first didn't take
    b.tap(DISMISS_EDIT); time.sleep(1)              # "Dismiss" on the post-install Edit-Home sheet
    b.press("home"); time.sleep(2)                  # exit any wiggle mode, back to page 1
    for _ in range(page - 1):
        b.drag(*NEXT_PAGE); time.sleep(3)
    # fresh idevicesyslog right before the tap, so the capture is alive for the launch window
    # even if the long-lived one dropped. It's a Boot.procs child (killed at b.stop()).
    launchlog = os.path.join(b.dir, "launch-syslog.log")
    b.procs.spawn(["idevicesyslog"], launchlog, env=b.env())
    time.sleep(3)
    b.tap(GRID(row, col))
    # 4. watch syslog for the app process; bounded wait.
    launched = False
    deadline = min(end, time.time() + 45)
    while time.time() < deadline:
        launched, _ = _syslog_has(launchlog, launch_needles)
        if launched:
            break
        time.sleep(2)
    # also accept the long-lived capture in case the fresh one dropped
    if not launched:
        launched, _ = _syslog_has(syslog, launch_needles)
    # 5. crash reports.
    crashdir = os.path.join(b.dir, "crash")
    os.makedirs(crashdir, exist_ok=True)
    b.run(["idevicecrashreport", "-e", crashdir], timeout=90)
    tokens = [t for t in (bundle.split(".")[-1].lower(), appname.lower()) if len(t) >= 3]
    crashes = [f for _, _, fs in os.walk(crashdir) for f in fs
               if f.lower().endswith((".crash", ".ips", ".plist"))
               and not f.lower().startswith(("lockdownd", "baseband", "stacks"))
               and any(t in f.lower() for t in tokens)]
    try:
        res["glishim"] = open(os.path.join(b.dir, "qemu.log"), errors="replace").read().count("[glishim] unimplemented")
    except OSError:
        pass
    if crashes:
        exc = ""
        for dp, _, fs in os.walk(crashdir):
            if crashes[0] in fs:
                m = re.search(r"Exception Type:\s*(.+)", open(os.path.join(dp, crashes[0]), errors="replace").read())
                exc = m.group(1).strip() if m else ""
                break
        res["verdict"] = "CRASH"
        res["note"] = ("%s | %s" % (exc, crashes[0]))[:140] if exc else crashes[0][:120]
    elif launched:
        res["verdict"], res["note"] = "LAUNCH", "syslog: app process started (%s)" % res["note"]
    else:
        res["verdict"], res["note"] = "NO-LAUNCH", "no app syslog line, no crash (%s)" % res["note"]
    return res


def launch_one(rg, cfg, ipa, r, install_only=False):
    """Install + launch one IPA on a fresh overlay. Returns dict: verdict, note, shot, crash, glishim."""
    tag = re.sub(r"[^A-Za-z0-9_.-]", "_", r["bundle"] or os.path.basename(ipa))[:60]
    res = {"file": r["file"], "bundle": r["bundle"], "name": r["name"], "family": r["family"],
           "verdict": "NO-BOOT", "note": "", "glishim": 0}
    b = rg.Boot(cfg, tag, usb=True)
    try:
        b.start()
        if not b.wait_mux():
            res["note"] = "usbmux never came up"
            return res
        # install
        ins = b.run(["ideviceinstaller", "install", ipa], timeout=200)
        out = (ins.stdout or "") + (ins.stderr or "")
        listed = r["bundle"] in (b.run(["ideviceinstaller", "list"], timeout=90).stdout or "")
        try:
            res["glishim"] = open(os.path.join(b.dir, "qemu.log"), errors="replace").read().count("[glishim] unimplemented")
        except OSError:
            pass
        if "Complete" not in out and not listed:
            res["verdict"] = "INSTALL-FAIL"
            res["note"] = out.strip().splitlines()[-1][:160] if out.strip() else "no installer output"
            return res
        if install_only:             # deterministic: install + list, no flaky touch
            res["verdict"] = "PASS-INSTALL" if listed else "INSTALL-FAIL"
            res["note"] = "installed and listed" if listed else "installer said Complete but not in list"
            return res
        # syslog, to confirm the app process starts (not pixels alone)
        syslog = os.path.join(b.dir, "syslog.log")
        b.procs.spawn(["idevicesyslog"], syslog, env=b.env())
        end = time.time() + cfg.boot_timeout - 30
        if getattr(cfg, "syslog_only", False):
            return syslog_launch(b, rg, cfg, r, res, syslog, end)
        # Event-driven navigation: wait on frame state, not fixed sleeps, so host load
        # doesn't matter. Overall bound = the qemu timeout.
        # 1. lock screen: serial marker (SpringBoard reached it) + a settled lit frame.
        ok, det = b.wait_lock_screen(timeout=max(30, end - time.time()))
        if not ok:
            res["verdict"], res["note"] = "NO-BOOT", "never reached lock screen (%s)" % det
            return res
        # sbservices only answers once SpringBoard is up (past the lock screen), so query
        # the icon layout now, not right after install.
        slot = icon_slot(b, r["bundle"]) or (2, 0, 0)     # fall back to page-2 slot(0,0)
        page, row, col = slot
        res["note"] = "slot p%d r%d c%d" % slot
        # 2. unlock: a real unlock is a BIG frame change (not a clock tick).
        lock = wait_stable(b, rg, tag, min(end, time.time() + 20), want_lit=True)
        home, unlocked = lock, False
        for _ in range(3):
            b.drag(rg.UNLOCK_FROM, rg.UNLOCK_TO)
            home = wait_stable(b, rg, tag, min(end, time.time() + 22), want_lit=True)
            if _framediff(lock, home, thresh=25):
                unlocked = True
                break
            b.press("home")
        if not unlocked:
            res["verdict"], res["note"] = "NO-LAUNCH", "could not unlock (%s)" % res["note"]
            return res
        b.tap(DISMISS_EDIT)                          # dismiss the install help sheet if present
        prev = wait_stable(b, rg, tag, min(end, time.time() + 20), want_lit=True)
        # 3. swipe to the app's page, one settled turn at a time.
        pa, pb = NEXT_PAGE
        for hop in range(page - 1):
            turned = False
            for _ in range(5):
                b.drag(pa, pb)
                nowf = wait_stable(b, rg, tag, min(end, time.time() + 20), want_lit=True)
                if _framediff(prev, nowf):
                    turned, prev = True, nowf
                    break
            if not turned:
                res["verdict"], res["note"] = "NAV-FAIL", "could not reach page %d (%s)" % (page, res["note"])
                return res
        before = prev
        syslen = os.path.getsize(syslog) if os.path.exists(syslog) else 0
        b.tap(GRID(row, col))
        after = wait_stable(b, rg, tag, min(end, time.time() + 40))   # settle on the app frame
        _, _, nz = rg.itqmp.shot(b.qmp, os.path.join(b.dir, "launch-" + tag + ".png"))
        res["shot"] = os.path.join(b.dir, "launch-" + tag + ".png")
        detail = "nz %.2f" % nz
        nonzero = nz
        changed = _framediff(before, after)
        # did the app's process appear in syslog after the tap?
        newlog = ""
        try:
            with open(syslog, errors="replace") as f:
                f.seek(syslen); newlog = f.read()
        except OSError:
            pass
        appname = (r["name"] or "").replace(" ", "")
        launched_log = any(t and t in newlog for t in (r["bundle"], r["bundle"].split(".")[-1], appname))
        # crash logs
        crashdir = os.path.join(b.dir, "crash")
        os.makedirs(crashdir, exist_ok=True)
        b.run(["idevicecrashreport", "-e", crashdir], timeout=90)
        # match the app's own crash reports; ignore always-pulled system artifacts
        # (lockdownd pairing plists, Baseband, Panics) and never match on an empty token.
        tokens = [t for t in (r["bundle"].split(".")[-1].lower(), (r["name"] or "").replace(" ", "").lower()) if len(t) >= 3]
        crashes = [f for _, _, fs in os.walk(crashdir) for f in fs
                   if f.lower().endswith((".crash", ".ips", ".plist"))
                   and not f.lower().startswith(("lockdownd", "baseband", "stacks"))
                   and any(t in f.lower() for t in tokens)]
        # GL gaps from the host log
        try:
            qlog = open(os.path.join(b.dir, "qemu.log"), errors="replace").read()
            res["glishim"] = qlog.count("[glishim] unimplemented")
        except OSError:
            pass
        if crashes:
            # pull the exception type out of the report so crashes group by cause
            exc = ""
            for dp, _, fs in os.walk(crashdir):
                if crashes[0] in fs:
                    txt = open(os.path.join(dp, crashes[0]), errors="replace").read()
                    m = re.search(r"Exception Type:\s*(.+)", txt) or re.search(r"Exception Codes:\s*(.+)", txt)
                    exc = m.group(1).strip() if m else ""
                    break
            res["verdict"] = "CRASH"
            res["note"] = ("%s | %s" % (exc, crashes[0]))[:140] if exc else crashes[0][:120]
        elif changed and launched_log:
            res["verdict"], res["note"] = "LAUNCH", "%s; syslog confirms" % detail
        elif changed:
            res["verdict"], res["note"] = "LAUNCH", "%s; no syslog line" % detail
        elif launched_log:
            res["verdict"], res["note"] = "LAUNCH", "syslog confirms; frame unchanged"
        elif nonzero < 0.02:
            res["verdict"], res["note"] = "NO-LAUNCH", "screen dark after tap"
        else:
            res["verdict"], res["note"] = "NO-LAUNCH", "no frame change, no syslog line"
    finally:
        b.stop()
    return res


def run_pass(a):
    rg = _load_regress()
    rg.itqmp.W, rg.itqmp.H = 1024, 768
    nand = os.path.realpath(a.nand or os.path.join(FILES, "userland", "golden-appsync"))
    out = a.out or os.path.join(FILES, "app-compat")
    os.makedirs(out, exist_ok=True)
    cfg = argparse.Namespace(out=out, kboot=os.path.join(FILES, "7B500", "k48-kboot.bin"),
                             nand=nand, qemu=os.path.join(ROOT, "build", "qemu-system-arm"),
                             usbmuxd=os.path.expanduser("~/Developer/usbmuxd-qemu-ipad1-net/src/usbmuxd"),
                             boot_timeout=a.boot_timeout, files=FILES, syslog_only=a.syslog_only)
    import json
    resdir = os.path.join(out, "results")
    os.makedirs(resdir, exist_ok=True)
    cands = [r for r in scan(a.dir) if is_candidate(r)]
    if a.only:
        cands = [r for r in cands if a.only.lower() in (r["file"] + r["bundle"] + r["name"]).lower()]
    # skip already-tested BEFORE applying --limit, so each batch takes the next N untested
    cands = [r for r in cands if a.redo or not os.path.exists(
        os.path.join(resdir, re.sub(r"[^A-Za-z0-9_.-]", "_", r["bundle"] or r["file"])[:60] + ".json"))]
    if a.limit:
        cands = cands[:a.limit]
    import concurrent.futures, threading
    print("running %d app(s) on %s, %d at a time" % (len(cands), nand, a.jobs))
    lock = threading.Lock()

    def one(r):
        ipa = None
        for dp, _, fs in os.walk(a.dir):
            if r["file"] in fs:
                ipa = os.path.join(dp, r["file"]); break
        res = launch_one(rg, cfg, ipa, r, install_only=a.install_only)
        key = re.sub(r"[^A-Za-z0-9_.-]", "_", r["bundle"] or r["file"])[:60]
        with lock:
            print("  %-12s %-30s %s" % (res["verdict"], res["bundle"], res["note"]), flush=True)
            json.dump(res, open(os.path.join(resdir, key + ".json"), "w"))
        return res

    if cands:
        with concurrent.futures.ThreadPoolExecutor(max_workers=a.jobs) as ex:
            list(ex.map(one, cands))
    # aggregate every per-app result recorded so far (batches accumulate)
    allres = [json.load(open(os.path.join(resdir, f))) for f in sorted(os.listdir(resdir)) if f.endswith(".json")]
    body = _results_md(allres, nand)
    open(os.path.join(out, "results.md"), "w").write(body)   # always beside the results
    default_out = os.path.join(FILES, "app-compat")
    if os.path.realpath(out) == os.path.realpath(default_out):   # only the canonical pass writes docs/
        open(os.path.join(ROOT, "docs/ipad1/app-compat-results.md"), "w").write(body)
        print("wrote docs/ipad1/app-compat-results.md")
    counts = {}
    for r in allres:
        counts[r["verdict"]] = counts.get(r["verdict"], 0) + 1
    print("%d total: %s" % (len(allres), counts))


def _results_md(allres, nand):
    order = {"PASS-INSTALL": 0, "LAUNCH": 0, "CRASH": 1, "NO-LAUNCH": 2, "NAV-FAIL": 3, "INSTALL-FAIL": 4, "NO-BOOT": 5}
    allres = sorted(allres, key=lambda r: (order.get(r["verdict"], 9), r.get("bundle", "")))
    counts = {}
    for r in allres:
        counts[r["verdict"]] = counts.get(r["verdict"], 0) + 1
    out = ["# iPad 1 / 3.2.2 app-compat results", "",
           "Store: `%s`. %d apps tested." % (nand, len(allres)), "",
           "Verdicts: " + ", ".join("%s %d" % (k, v) for k, v in sorted(counts.items())), ""]
    # failure backlog grouped by cause (the fix list)
    crashes = [r for r in allres if r["verdict"] == "CRASH"]
    nolaunch = [r for r in allres if r["verdict"] == "NO-LAUNCH"]
    glgaps = [r for r in allres if r.get("glishim", 0) > 0 and r["verdict"] in ("LAUNCH", "NO-LAUNCH", "CRASH")]
    if crashes or nolaunch or glgaps:
        out += ["## Failure backlog (by cause)", ""]
    if glgaps:
        out += ["### GL: unimplemented entry points (need glishim work)", ""]
        for r in sorted(glgaps, key=lambda r: -r.get("glishim", 0)):
            out.append("- %s (`%s`): %d [glishim] unimplemented, verdict %s" % (
                r.get("name") or r["file"], r["bundle"], r["glishim"], r["verdict"]))
        out.append("")
    if crashes:
        out += ["### Crashes", ""]
        for r in crashes:
            out.append("- %s (`%s`): %s" % (r.get("name") or r["file"], r["bundle"], r.get("note", "")))
        out.append("")
    if nolaunch:
        out += ["### No launch (installed, did not come to foreground)", ""]
        for r in nolaunch:
            out.append("- %s (`%s`): %s" % (r.get("name") or r["file"], r["bundle"], r.get("note", "")))
        out.append("")
    out += ["## All results", "",
            "| verdict | app | bundle | family | GL gaps | note |", "|---|---|---|---|---|---|"]
    for r in allres:
        out.append("| %s | %s | `%s` | %s | %d | %s |" % (
            r["verdict"], r.get("name") or r.get("file", ""), r.get("bundle", ""),
            r.get("family", ""), r.get("glishim", 0), r.get("note", "")))
    return "\n".join(out) + "\n"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", nargs="?", default="inventory", choices=["inventory", "run"])
    ap.add_argument("dir", nargs="?", default=DEFAULT_DIR)
    ap.add_argument("--md")
    ap.add_argument("--only", help="run: substring filter on file/bundle/name")
    ap.add_argument("--limit", type=int, help="run: cap number of apps")
    ap.add_argument("--nand", help="run: NAND store (default golden-appsync)")
    ap.add_argument("--out", help="run: output dir")
    ap.add_argument("--boot-timeout", type=int, default=240)
    ap.add_argument("--jobs", type=int, default=1, help="run: apps in parallel (each its own overlay + ports)")
    ap.add_argument("--install-only", action="store_true", help="run: install + list only, no launch (deterministic)")
    ap.add_argument("--syslog-only", action="store_true", help="run: launch, verdict from idevicesyslog + crash logs, no screendumps")
    ap.add_argument("--redo", action="store_true", help="run: re-test apps that already have a result")
    ap.add_argument("--selfcheck", action="store_true")
    a = ap.parse_args()
    if a.selfcheck:
        return selfcheck()
    if a.cmd == "run":
        return run_pass(a)
    rows = scan(a.dir)
    cand = [r for r in rows if is_candidate(r)]
    enc = [r for r in rows if r["encrypted"]]
    err = [r for r in rows if r["error"]]
    print("%d IPAs: %d candidates, %d encrypted, %d unreadable" % (len(rows), len(cand), len(enc), len(err)))
    for r in cand:
        print("  candidate %-40s %-32s minOS %s %s %s" % (
            r["file"][:40], r["bundle"], r["minos"] or "-", r["family"], ",".join(r["archs"])))
    if a.md:
        os.makedirs(os.path.dirname(a.md), exist_ok=True)
        open(a.md, "w").write(to_md(rows, a.dir))
        print("wrote", a.md)


if __name__ == "__main__":
    sys.exit(main())
