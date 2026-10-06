#!/usr/bin/env python3
"""Apple's debugserver on a kboot board, from a real DeveloperDiskImage (docs/guest-debug.md).

    tests/ipad1/ddi-check.py --machine iPod-Touch-4G --device DEV --product-version 6.1.6 \\
        --ddi DIR [--overlay OVL] [--sysroot ROOT] [--ipa IPA] --out OUT

DIR holds DeveloperDiskImage.dmg and its .signature (Xcode's DeviceSupport/<version>). Setup Assistant is walked
(app-install.py's walk_setup) when it is up; SpringBoard launches no app until it is done.

  mount   the DDI through mobile_image_mounter with its signature (iOS < 7: uploaded over AFC, then MountImage)
  launch  the IPA (default the Harness) signed with get-task-allow, as Xcode signs a development build, installed
          and launched through the guest agent
  attach  com.apple.debugserver through lockdown (idevicedebugserverproxy) and lldb (imgtools/lldb/dsattach.py):
          attach, a breakpoint where the main thread's mach_msg returns (woken by a touch), backtrace into the app,
          continue, detach; the app must still be running afterwards
  --expect-refused  the negative case: the IPA signed without get-task-allow (ldid -S, no entitlements);
          debugserver must refuse the attach ("attach failed") and the app must keep running
"""
import argparse, ctypes, ctypes.util, importlib.util, os, re, shutil, subprocess, sys, time, zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
HARNESS = os.path.join(ROOT, "contrib/it-harness/build/Harness.ipa")
spec = importlib.util.spec_from_file_location("ipad1_regress", os.path.join(HERE, "regress.py"))
rg = importlib.util.module_from_spec(spec)
spec.loader.exec_module(rg)
spec = importlib.util.spec_from_file_location("ipad1_app_install", os.path.join(HERE, "app-install.py"))
ai = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ai)
ai.rg = rg                     # walk_setup scales its taps by this module's panel size
GET_TASK_ALLOW = b"""<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict><key>get-task-allow</key><true/></dict></plist>
"""


def lib(name):
    path = subprocess.run(["pkg-config", "--variable=libdir", name], capture_output=True, text=True).stdout.strip()
    stem = {"libimobiledevice-1.0": "libimobiledevice-1.0", "libplist-2.0": "libplist-2.0"}[name]
    return ctypes.CDLL(os.path.join(path, stem + ".dylib"))


def mount_image(mux_port, udid, signature):
    """MountImage of AFC's PublicStaging/staging.dimage (libimobiledevice 1.4.0's ideviceimagemounter never sends it
    on iOS < 7: its AFC branch keeps an earlier call's error). Returns (error, reply as XML)."""
    os.environ["USBMUXD_SOCKET_ADDRESS"] = "127.0.0.1:%d" % mux_port
    imd, plist = lib("libimobiledevice-1.0"), lib("libplist-2.0")
    dev, mim, result = ctypes.c_void_p(), ctypes.c_void_p(), ctypes.c_void_p()
    if imd.idevice_new(ctypes.byref(dev), udid.encode()) or \
            imd.mobile_image_mounter_start_service(dev, ctypes.byref(mim), b"ddi-check"):
        return -1, "no mobile_image_mounter"
    sig = open(signature, "rb").read()
    e = imd.mobile_image_mounter_mount_image(mim, b"/var/mobile/Media/PublicStaging/staging.dimage", sig, len(sig),
                                             b"Developer", ctypes.byref(result))
    xml, n = ctypes.c_char_p(), ctypes.c_uint32()
    if result:
        plist.plist_to_xml(result, ctypes.byref(xml), ctypes.byref(n))
    imd.mobile_image_mounter_hangup(mim)
    imd.mobile_image_mounter_free(mim)
    imd.idevice_free(dev)
    return e, (xml.value or b"").decode(errors="replace")


def dev_signed(ipa, out, debuggable=True):
    """A copy of `ipa` whose executable carries get-task-allow; (path, bundle id, host copy of the executable)."""
    import plistlib
    work = os.path.join(out, "ipa")
    shutil.rmtree(work, ignore_errors=True)
    with zipfile.ZipFile(ipa) as z:
        z.extractall(work)
    app = next(os.path.join(work, "Payload", d) for d in os.listdir(os.path.join(work, "Payload")) if d.endswith(".app"))
    info = plistlib.load(open(os.path.join(app, "Info.plist"), "rb"))
    exe = os.path.join(app, info["CFBundleExecutable"])
    ent = os.path.join(out, "get-task-allow.plist")
    open(ent, "wb").write(GET_TASK_ALLOW)
    subprocess.run(["ldid", "-S" + ent if debuggable else "-S", exe], check=True)   # -S alone: no entitlements
    signed = os.path.join(out, "dev-signed.ipa")
    if os.path.exists(signed):
        os.unlink(signed)
    with zipfile.ZipFile(signed, "w", zipfile.ZIP_DEFLATED) as z:
        for base, _, files in os.walk(work):
            for f in files:
                p = os.path.join(base, f)
                z.write(p, os.path.relpath(p, work))
    return signed, info["CFBundleIdentifier"], exe


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    rg.ipad1_boot.add_arguments(ap)
    ap.add_argument("--nand")
    ap.add_argument("--qemu", default=os.path.join(ROOT, "build/qemu-system-arm"))
    ap.add_argument("--usbmuxd", default=rg.USBMUXD)
    ap.add_argument("--product-version", required=True)
    ap.add_argument("--boot-timeout", type=int, default=1500)
    ap.add_argument("--ddi", required=True)
    ap.add_argument("--overlay", help="a NAND overlay used in place (kept, shut down cleanly at the end): the first run "
                    "on a new one walks Setup, later runs start past it")
    ap.add_argument("--sysroot", help="host libraries for lldb (dsc_extract.py of the build's shared cache, plus "
                    "usr/lib/dyld); without it lldb reads every library from guest memory (about 100 s)")
    ap.add_argument("--ipa", default=HARNESS)
    ap.add_argument("--port", type=int, default=23970)
    ap.add_argument("--expect-refused", action="store_true",
                    help="the negative case: the IPA signed without get-task-allow; debugserver must refuse the attach "
                    "and the app must keep running")
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    rg.itqmp.W, rg.itqmp.H = rg.ipad1_boot.MACHINES[a.machine]
    if a.machine in rg.ipad1_boot.PORTRAIT:
        rg.LIT_MIN_FRACTION = 0.2
        rg.UNLOCK_FROM, rg.UNLOCK_TO = rg.portrait_unlock()
        if rg.itqmp.W < rg.itqmp.H:
            rg.portrait_layout()
    rg.device_args(a)
    rg.ipod.START = time.time()
    os.makedirs(a.out, exist_ok=True)
    dmg = os.path.join(a.ddi, "DeveloperDiskImage.dmg")
    major = int(a.product_version.split(".")[0])
    results = []

    def step(name, ok, detail):
        results.append((name, ok, detail))
        rg.log("%s %s: %s" % ("PASS" if ok else "FAIL", name, detail))
        return ok

    b = rg.Boot(a, "ddi", overlay=a.overlay, usb=True)
    if a.overlay:
        # The host's pair records live next to the overlay: a device that paired once asks 7.x's "Trust This
        # Computer?" again for a new host, and nothing answers it here
        conf = os.path.abspath(a.overlay).rstrip("/") + ".conf"
        os.makedirs(conf, exist_ok=True)
        shutil.rmtree(os.path.join(b.dir, "conf"), ignore_errors=True)
        os.symlink(conf, os.path.join(b.dir, "conf"))
    proxy = None
    try:
        b.start()
        ok, det = b.wait_lock_screen(timeout=500)
        if not step("lock", ok, det):
            return 1
        if major >= 7:
            # 7.x: it_boot starts the guest agent (the unlock asks it), and file creation on the data volume
            # (lockdownd's pairing escrow keybag, AFC's staging file) can stall until it has run
            t0 = time.time()
            while time.time() - t0 < 240 and "it_boot: package" not in open(b.serial, errors="replace").read():
                time.sleep(3)
            if not step("it_boot", "it_boot: package" in open(b.serial, errors="replace").read(),
                        "reported %.0f s after the lock screen" % (time.time() - t0)):
                return 1
        front, hello = "", False
        for _ in range(4):
            b.press("home"); time.sleep(1.5); b.drag(rg.UNLOCK_FROM, rg.UNLOCK_TO); time.sleep(4)
            st, out = rg.itqmp.agent(b.qmp, "frontmost")
            front = out.decode(errors="replace").split("\n")[0]
            st, lock = rg.itqmp.agent(b.qmp, "lockstatus")
            # 7.x's Setup starts on a Hello screen that lockstatus calls locked; its slide opens the language page
            hello = major >= 7 and b"locked=0" not in lock and "English" in ai.ocr_upright(b.shot("opened"))
            if b"locked=0" in lock or hello:
                break
        if not step("unlock", b"locked=0" in lock or hello,
                    "front %s, %s%s" % (front, lock.decode(errors="replace").strip(), ", Hello slid" if hello else "")):
            return 1
        if hello or front == "com.apple.purplebuddy":     # Setup Assistant: SpringBoard launches no app while it is up
            if not step("setup", *ai.walk_setup(b, step)):
                return 1
        if not step("lockdown", b.wait_mux(timeout=300), "ProductVersion %s, udid %s" % (a.product_version, b.udid)):
            return 1

        # mount
        if major >= 7:
            r = b.run(["ideviceimagemounter", "mount", dmg, dmg + ".signature"], timeout=600)
            detail = (r.stdout + r.stderr).strip().splitlines()[-1:]
        else:
            r = b.run(["ideviceimagemounter", "mount", dmg, dmg + ".signature"], timeout=600)   # the AFC upload
            e, xml = mount_image(b.mux_port, b.udid, dmg + ".signature")
            detail = ["ideviceimagemounter: %s; MountImage %d %s" % ((r.stdout + r.stderr).strip().splitlines()[-1:], e,
                                                                     " ".join(re.findall(r"<string>([^<]*)</string>", xml)))]
        listing = b.run(["ideviceimagemounter", "list"], timeout=60).stdout
        if not step("mount", "ImagePresent: true" in listing, "; ".join(detail) + "; " + " ".join(listing.split()[:6])):
            return 1

        # launch
        ipa, bundle, exe = dev_signed(a.ipa, a.out, debuggable=not a.expect_refused)
        r = b.run(["ideviceinstaller", "install", ipa], timeout=600)
        if not step("install", "Complete" in r.stdout, (r.stdout + r.stderr).strip().splitlines()[-1:]):
            return 1
        rg.itqmp.agent(b.qmp, "launch", bundle)
        time.sleep(6)

        def pid():
            st, out = rg.itqmp.agent(b.qmp, "spawn", "", b"/bin/launchctl\0list\0")
            m = re.search(r"^(\d+)\s+\S+\s+UIKitApplication:%s\[" % re.escape(bundle), out.decode(errors="replace"), re.M)
            return m and m.group(1)
        app = pid()
        if not step("launch", bool(app), "%s pid %s" % (bundle, app)):
            return 1

        # attach
        proxy = subprocess.Popen(["idevicedebugserverproxy", "-u", b.udid, str(a.port)], env=b.env(),
                                 stdout=open(os.path.join(b.dir, "dsproxy.log"), "w"), stderr=subprocess.STDOUT,
                                 stdin=subprocess.DEVNULL)
        time.sleep(2)
        wake = os.path.join(b.dir, "wake")
        if os.path.exists(wake):
            os.unlink(wake)
        env = dict(os.environ, DSPORT=str(a.port), DSPID=app, DSWAKE=wake, DSLOG=os.path.join(b.dir, "gdb-remote.log"), DSSYSROOT=a.sysroot or "")
        log = open(os.path.join(b.dir, "lldb.log"), "w")
        lldb = subprocess.Popen(["lldb", "-b", "-o", "command script import " + os.path.join(ROOT, "imgtools/lldb/dsattach.py")],
                                env=env, stdout=log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
        t0 = time.time()
        while lldb.poll() is None and time.time() - t0 < 300:
            if os.path.exists(wake):
                os.unlink(wake)
                time.sleep(1)
                b.tap((rg.itqmp.W // 2, 20))     # the status bar: wakes the run loop, changes nothing
            time.sleep(1)
        if lldb.poll() is None:
            lldb.kill()
        out = open(os.path.join(b.dir, "lldb.log"), errors="replace").read()
        ds = dict((m.group(1), m.group(2)) for m in re.finditer(r"^ds: (\w+) (.*)$", out, re.M))
        frames = re.findall(r"^ds: frame \d+ frame #\d+: 0x[0-9a-f]+ (\S*)`", out, re.M)
        if a.expect_refused:
            att = ds.get("attach", "none")
            step("refused", "attach failed" in att.lower() and not att.startswith("ok"), att)
            time.sleep(3)
            alive = pid()
            step("alive", alive == app, "pid %s after the refused attach (was %s)" % (alive, app))
            return 0 if all(ok for _, ok, _ in results) else 1
        step("attach", ds.get("attach", "").startswith("ok"), ds.get("attach", "none"))
        step("break", ds.get("stop", "").startswith("stopped at-breakpoint") and os.path.basename(exe) in frames,
             "%s; frames %s" % (ds.get("stop"), " > ".join(frames)))
        step("detach", ds.get("detach", "").startswith("ok"), "continue %s, detach %s" % (ds.get("continue"), ds.get("detach")))
        time.sleep(3)
        alive = pid()
        st, front = rg.itqmp.agent(b.qmp, "frontmost")
        step("alive", alive == app, "pid %s after detach (was %s); front %s" % (alive, app, front.decode(errors="replace").split("\n")[0]))
    finally:
        if proxy:
            proxy.kill()
        if a.overlay and getattr(b, "qmp", None):
            b.powerdown()     # a clean shutdown: the overlay keeps Setup's result for the next run
        b.stop()
        for name, ok, det in results:
            print("%s  %-8s %s" % ("PASS" if ok else "FAIL", name, det))
    return 0 if results and all(ok for _, ok, _ in results) else 1


if __name__ == "__main__":
    sys.exit(main())
