#!/usr/bin/env python3
"""Seal a freshly built ipad1 NAND store: one clean shutdown, so every later boot finds a valid FTL context.

    ipad1_seal.py STORE [--qemu build/qemu-system-arm] [--kboot FILES/7B500/k48-kboot.bin] [--die-id 0xW2:0xW3]

STORE must come from a system.img baked with `ipad1_rootfs.py bake --seal`. Its it_seal job halts the
first boot cleanly (reboot(2): sync, unmount, FTL close) and deletes itself. Without that, every boot of
the store logs "CXT is not valid . Performing full NAND R/O restore" and rescans the NAND for ~13 s.

Step 1 boots STORE in place and waits for the guest to halt (QEMU exits on the PMU power-off write).
Step 2 boots it again read-only, with a throwaway overlay so STORE stays sealed, and requires FTL_Open
without the rescan. Then chmod -R a-w STORE (docs/ipad1/userland-boot.md).
"""
import argparse, os, shutil, subprocess, sys, tempfile, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FILES = os.path.expanduser("~/Developer/qemu-ios-files/ipad1/7B500")
RESCAN = "CXT is not valid"
FTL_OPEN = "[FTL:MSG] FTL_Open"
HALTING = "it_seal: halting"


def boot(qemu, kboot, machine_extra, serial, stop, timeout):
    """Run QEMU until it exits or stop(serial text) is true; returns (exited, seconds, text)."""
    cmd = [qemu, "-machine", f"ipad1,kboot={kboot},{machine_extra}", "-display", "none",
           "-monitor", "none", "-serial", f"file:{serial}"]
    t0 = time.monotonic()
    p = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    text = ""
    try:
        while p.poll() is None and time.monotonic() - t0 < timeout:
            time.sleep(0.5)
            text = open(serial, errors="replace").read() if os.path.exists(serial) else ""
            if stop and stop(text):
                break
    finally:
        exited = p.poll() is not None
        if not exited:
            p.kill()
            p.wait()
    text = open(serial, errors="replace").read() if os.path.exists(serial) else text
    return exited, time.monotonic() - t0, text


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("store")
    ap.add_argument("--qemu", default=f"{ROOT}/build/qemu-system-arm")
    ap.add_argument("--kboot", default=f"{FILES}/k48-kboot.bin")
    ap.add_argument("--timeout", type=int, default=300)
    ap.add_argument("--die-id", help="machine die-id, as in the kboot bundle's identity")
    ap.add_argument("--nor-rw", help="private writable NOR copy; the sealing boot's effaceable writes persist here")
    a = ap.parse_args()
    store = os.path.abspath(a.store)
    die = f",die-id={a.die_id}" if a.die_id else ""
    if a.nor_rw:
        die += f",nor-rw={os.path.abspath(a.nor_rw)}"
    td = tempfile.mkdtemp(prefix="ipad1-seal-")
    try:
        exited, t, text = boot(a.qemu, a.kboot, f"nand={store}{die}", f"{td}/seal.log", None, a.timeout)
        if not exited or HALTING not in text:
            sys.exit(f"seal boot: {'no clean halt' if not exited else 'QEMU exited without it_seal'} after "
                     f"{t:.0f}s (was the system.img baked with --seal?); serial in {td}/seal.log")
        print(f"sealing boot halted cleanly after {t:.0f}s")
        os.mkdir(f"{td}/overlay")
        _, t, text = boot(a.qemu, a.kboot, f"nand={store},nand-overlay={td}/overlay{die}", f"{td}/check.log",
                          lambda s: FTL_OPEN in s, 120)
        if FTL_OPEN not in text or RESCAN in text:
            sys.exit(f"check boot: {'still rescans' if RESCAN in text else 'no FTL_Open'}; serial in {td}/check.log")
        print(f"check boot: valid FTL context, FTL_Open after {t:.1f}s")
        print(next(l for l in text.splitlines() if FTL_OPEN in l))
    except SystemExit:
        raise
    shutil.rmtree(td, ignore_errors=True)


if __name__ == "__main__":
    main()
