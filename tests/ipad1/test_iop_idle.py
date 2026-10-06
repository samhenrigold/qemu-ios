#!/usr/bin/env python3
"""An idle A4 board leaves the host idle: the IOP core sleeps in its firmware's idle loop.

    tests/ipad1/test_iop_idle.py [--machine ipad1] [--device DIR] [--kboot ...] [--qemu Q]

Boots the device to its lock screen (regress.py's Boot, the IOP core on), waits for the panel to sleep, and
reads QEMU's CPU time over 10 s: under half a host core passes. EmbeddedIOP's idle task is `for (;;) yield();`
with no WFI, so without the idle-loop hint (s5l8930_iop_core.c, ArchCPU::idle_loop_pc) the IOP core spun at
a full core (iPad 3.2.2: 104%, N81 5.0: 180%, N90 6.0b1: 183%; 7, 3 and 12% with it). Also fails on a kernel
panic: a halted IOP that never woke (a core left PSCI_OFF) timed out the AP's FMI command.
"""
import os, re, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.argv, ARGS = sys.argv[:1], sys.argv[1:]
import regress as rg, ipad1_boot, itqmp, argparse  # noqa: E402


def cpu_seconds(pid):
    t = subprocess.run(["ps", "-o", "cputime=", "-p", str(pid)], capture_output=True, text=True).stdout.strip()
    parts = [float(x) for x in re.split("[:]", t)]
    return sum(v * 60 ** i for i, v in enumerate(reversed(parts)))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ipad1_boot.add_arguments(ap)
    ap.add_argument("--qemu", default=os.path.join(rg.ROOT, "build/qemu-system-arm"))
    ap.add_argument("--product-version")
    ap.add_argument("--nand")
    ap.add_argument("--boot-timeout", type=int, default=900)
    ap.add_argument("--usbmuxd", default=rg.USBMUXD)
    ap.add_argument("--guest-package")
    a = ap.parse_args(ARGS)
    itqmp.W, itqmp.H = ipad1_boot.MACHINES[a.machine]
    if a.machine in ipad1_boot.PORTRAIT:
        rg.LIT_MIN_FRACTION = 0.2
    rg.device_args(a)
    rg.ipod.START = time.time()
    a.out = tempfile.mkdtemp(prefix="iop-idle-")
    b = rg.Boot(a, "idle", usb=False)
    b.start()
    try:
        ok, detail = b.wait_lock_screen()
        if not ok:
            sys.exit("FAIL: " + detail)
        time.sleep(20)                                  # the lock screen's panel sleeps
        pid = int(subprocess.run(["pgrep", "-P", str(b.qemu.pid)], capture_output=True, text=True).stdout.split()[0])
        c0, t0 = cpu_seconds(pid), time.time()
        time.sleep(10)
        load = (cpu_seconds(pid) - c0) / (time.time() - t0)
        b.qmp.cmd("quit")
    finally:
        b.stop()
    panicked = "panic(" in open(b.serial, errors="replace").read()
    ok = load < 0.5 and not panicked
    print("%s: idle at %.0f%% of a host core%s (out %s)" % ("PASS" if ok else "FAIL", load * 100,
          ", kernel panic" if panicked else "", a.out))
    if ok:
        import shutil
        shutil.rmtree(a.out, ignore_errors=True)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
