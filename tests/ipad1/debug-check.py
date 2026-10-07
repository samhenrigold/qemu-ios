#!/usr/bin/env python3
"""Guest debugging check for the kboot boards (docs/guest-debug.md): boot with a gdbstub, unlock, then two lldb
passes through imgtools/lldb/xnu.py.

    tests/ipad1/debug-check.py --machine n88 --device DEV --product-version 6.1.6 --kernel KERNELCACHE \\
        --sysroot ROOT --out OUT [--port 23946]

kernel: xnu-procs must list SpringBoard (offsets found in the kernel itself). user: xnu-images --pid <SpringBoard>
adds its images from ROOT (a host copy of the rootfs with the shared cache extracted by dsc_extract.py), and
`xnu-break SpringBoard mach_msg` must stop in SpringBoard with a symbolized libsystem frame. Logs in OUT.
"""
import argparse, importlib.util, os, re, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
spec = importlib.util.spec_from_file_location("ipad1_regress", os.path.join(HERE, "regress.py"))
rg = importlib.util.module_from_spec(spec)
spec.loader.exec_module(rg)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    rg.ipad1_boot.add_arguments(ap)
    ap.add_argument("--nand")
    ap.add_argument("--qemu", default=os.path.join(ROOT, "build/qemu-system-arm"))
    ap.add_argument("--usbmuxd", default=rg.USBMUXD)
    ap.add_argument("--product-version")
    ap.add_argument("--boot-timeout", type=int, default=560)
    ap.add_argument("--kernel", required=True)
    ap.add_argument("--sysroot", required=True)
    ap.add_argument("--port", type=int, default=23946)
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
    os.environ["IPAD1_QEMU_EXTRA"] = (os.environ.get("IPAD1_QEMU_EXTRA", "") + " -gdb tcp:127.0.0.1:%d" % a.port).strip()
    os.makedirs(a.out, exist_ok=True)

    def lldb(name, cmds):
        script = os.path.join(a.out, name + ".lldb")
        with open(script, "w") as f:
            f.write("\n".join(["target create --arch armv7-apple-ios " + a.kernel, "gdb-remote 127.0.0.1:%d" % a.port,
                               "command script import " + os.path.join(ROOT, "imgtools/lldb/xnu.py")]
                              + cmds + ["detach"]) + "\n")
        p = subprocess.run(["timeout", "240", "lldb", "-b", "-s", script], capture_output=True, text=True)
        with open(os.path.join(a.out, name + ".log"), "w") as f:
            f.write(p.stdout + p.stderr)
        return p.stdout

    results = []
    b = rg.Boot(a, "debug", usb=True)
    try:
        b.start()
        ok, det = b.wait_lock_screen(timeout=400)
        results.append(("lock", ok, det))
        if ok:
            b.press("home")
            time.sleep(1.5)
            b.drag(rg.UNLOCK_FROM, rg.UNLOCK_TO)
            time.sleep(8)
            out = lldb("kernel", ["xnu-offsets", "xnu-procs", "xnu-current", "bt 6"])
            m = re.search(r"^\s*(\d+)\s+SpringBoard\s", out, re.M)
            results.append(("kernel", bool(m), "xnu-procs: SpringBoard pid %s" % (m and m.group(1))))
            if m:
                out = lldb("user", ["xnu-images --sysroot %s --pid %s" % (a.sysroot, m.group(1)),
                                    "xnu-break SpringBoard mach_msg", "continue", "xnu-current", "bt 8",
                                    "breakpoint delete 1"])
                frame = re.search(r"frame #0: 0x[0-9a-f]+ (\S+)`mach_msg", out)
                cur = re.search(r"user mode, pid %s SpringBoard" % m.group(1), out)
                added = out.count("(added")
                results.append(("user", bool(frame and cur), "%d images added; stop %s in SpringBoard: %s" % (
                    added, frame.group(1) + "`mach_msg" if frame else "none", bool(cur))))
    finally:
        b.stop()
        for name, ok, det in results:
            print("%s  %-7s %s" % ("PASS" if ok else "FAIL", name, det))
    return 0 if len(results) == 3 and all(ok for _, ok, _ in results) else 1


if __name__ == "__main__":
    sys.exit(main())
