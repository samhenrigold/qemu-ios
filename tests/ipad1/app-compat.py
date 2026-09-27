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
# Portrait UI on the 1024x768 landscape scanout (measured in the AppSync proof runs).
DISMISS_EDIT = (408, 470)        # "Dismiss" on the install's Edit-Home-Screen help sheet
PAGE2_SWIPE = ((512, 680), (512, 150))   # page 1 -> page 2 (the freshly installed icon)
NEW_ICON = (895, 115)            # first slot on page 2 = the just-installed app
LAUNCH_WAIT = 9


def _sample(rg, ppm, step=997):
    """Flat list of sampled pixel bytes from a ppm, or [] if unreadable."""
    try:
        _, _, pix = rg.itqmp.read_ppm(ppm)
        return list(pix[::step])
    except Exception:
        return []


def _framediff(a, b, thresh=8):
    """True if two sampled frames differ meaningfully (mean abs byte diff > thresh)."""
    if not a or not b or len(a) != len(b):
        return True
    d = sum(abs(x - y) for x, y in zip(a, b)) / len(a)
    return d > thresh


def _load_regress():
    spec = importlib.util.spec_from_file_location("ipad_regress", os.path.join(HERE, "regress.py"))
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    m.ipod.START = time.time()   # ipod.log() reads this; normally set in ipod.main()
    return m


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
        # to the home screen, then launch the new icon. Touch on this store is flaky,
        # so verify each step by frame change and retry (no guest agent on ipad1).
        b.wait_lock_screen()
        cur = _sample(rg, b.shot("s0-" + tag))
        for _ in range(4):           # unlock until the frame leaves the lock screen
            b.drag(rg.UNLOCK_FROM, rg.UNLOCK_TO); time.sleep(2)
            nxt = _sample(rg, b.shot("s1-" + tag))
            if _framediff(cur, nxt):
                break
            b.press("home"); time.sleep(1)
        b.tap(DISMISS_EDIT); time.sleep(1)     # dismiss the install help sheet if present
        page1 = _sample(rg, b.shot("page1-" + tag))
        reached = False
        for _ in range(4):           # swipe to page 2 (the new icon) until the page changes
            b.drag(*PAGE2_SWIPE); time.sleep(2)
            page2 = _sample(rg, b.shot("iconpage-" + tag))
            if _framediff(page1, page2):
                reached = True
                break
        if not reached:              # never left page 1 — don't mis-tap a stock icon
            res["verdict"], res["note"] = "NAV-FAIL", "could not reach the new app's page"
            return res
        before = page2
        b.tap(NEW_ICON)
        time.sleep(LAUNCH_WAIT)
        lit, detail = b.picture("launch-" + tag)
        res["shot"] = os.path.join(b.dir, "launch-" + tag + ".ppm")
        after = _sample(rg, res["shot"])
        # LAUNCH = the frame changed from the icon page (an app is frontmost, incl. the
        # black-bordered 2x compat window). A near-identical frame = tap bounced back to
        # the home screen. A dark frame = nothing came up / panel slept.
        nonzero = sum(1 for v in after if v > 12) / max(1, len(after))
        changed = _framediff(before, after)
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
            res["verdict"], res["note"] = "CRASH", crashes[0][:120]
        elif nonzero < 0.02:
            res["verdict"], res["note"] = "NO-LAUNCH", "screen dark after tap (%s)" % detail
        elif not changed:
            res["verdict"], res["note"] = "NO-LAUNCH", "frame unchanged from icon page (bounced?)"
        else:
            res["verdict"], res["note"] = "LAUNCH", detail
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
                             boot_timeout=a.boot_timeout, files=FILES)
    cands = [r for r in scan(a.dir) if is_candidate(r)]
    if a.only:
        cands = [r for r in cands if a.only.lower() in (r["file"] + r["bundle"] + r["name"]).lower()]
    if a.limit:
        cands = cands[:a.limit]
    print("running %d app(s) on %s" % (len(cands), nand))
    results = []
    for r in cands:
        ipa = None
        for dp, _, fs in os.walk(a.dir):
            if r["file"] in fs:
                ipa = os.path.join(dp, r["file"]); break
        res = launch_one(rg, cfg, ipa, r, install_only=a.install_only)
        print("  %-10s %-30s %s" % (res["verdict"], res["bundle"], res["note"]))
        results.append(res)
    md = os.path.join(ROOT, "docs/ipad1/app-compat-results.md")
    with open(md, "w") as f:
        f.write("# iPad 1 / 3.2.2 app-launch results\n\nStore: `%s`. %d apps.\n\n" % (nand, len(results)))
        f.write("| verdict | app | bundle | family | GL gaps | note |\n|---|---|---|---|---|---|\n")
        for r in results:
            f.write("| %s | %s | `%s` | %s | %d | %s |\n" % (
                r["verdict"], r["name"] or r["file"], r["bundle"], r["family"], r.get("glishim", 0), r["note"]))
    print("wrote", md)


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
    ap.add_argument("--install-only", action="store_true", help="run: install + list only, no launch (deterministic)")
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
