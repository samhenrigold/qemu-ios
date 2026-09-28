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
import re, argparse, os, shutil, subprocess, sys, tempfile, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FILES = os.path.expanduser("~/Developer/qemu-ios-files/ipad1/7B500")
RESCAN = "CXT is not valid"
# Matched with newlines removed: other kernel messages interleave with this line on the serial log.
FTL_OPEN_RE = re.compile(r"FTL_Open\s*\[OK\]")
def ftl_open(text): return FTL_OPEN_RE.search(text.replace("\n", "")) is not None
HALTING = "it_seal: halting"


def boot(qemu, boot_options, machine_extra, serial, stop, timeout):
    """Run QEMU until it exits or stop(serial text) is true; returns (exited, seconds, text)."""
    cmd = [qemu, "-machine", f"ipad1,{boot_options},{machine_extra}", "-display", "none", "-audio", "driver=none",
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
    boot_group = ap.add_mutually_exclusive_group(required=True)
    boot_group.add_argument("--kboot", help="explicit direct-kernel bring-up fallback")
    boot_group.add_argument("--iboot")
    ap.add_argument("--gid-blobs")
    ap.add_argument("--timeout", type=int, default=300)
    ap.add_argument("--die-id", help="machine die-id, as in the kboot bundle's identity")
    ap.add_argument("--nor-rw", help="private writable NOR copy; the sealing boot's effaceable writes persist here")
    a = ap.parse_args()
    if a.iboot and not (a.gid_blobs and a.nor_rw):
        ap.error("--iboot needs --gid-blobs and --nor-rw")
    boot_options = f"iboot={a.iboot},gid-blobs={a.gid_blobs}" if a.iboot else f"kboot={a.kboot}"
    store = os.path.abspath(a.store)
    die = f",die-id={a.die_id}" if a.die_id else ""
    if a.nor_rw:
        die += f",nor-rw={os.path.abspath(a.nor_rw)}"
    td = tempfile.mkdtemp(prefix="ipad1-seal-")
    try:
        exited, t, text = boot(a.qemu, boot_options, f"nand={store}{die}", f"{td}/seal.log", None, a.timeout)
        if not exited or HALTING not in text:
            sys.exit(f"seal boot: {'no clean halt' if not exited else 'QEMU exited without it_seal'} after "
                     f"{t:.0f}s (was the system.img baked with --seal?); serial in {td}/seal.log")
        print(f"sealing boot halted cleanly after {t:.0f}s")
        os.mkdir(f"{td}/overlay")
        _, t, text = boot(a.qemu, boot_options, f"nand={store},nand-overlay={td}/overlay{die}", f"{td}/check.log",
                          ftl_open, 120)
        if not ftl_open(text) or RESCAN in text:
            sys.exit(f"check boot: {'still rescans' if RESCAN in text else 'no FTL_Open'}; serial in {td}/check.log")
        print(f"check boot: valid FTL context, FTL_Open after {t:.1f}s")
        print(next((l for l in text.splitlines() if "FTL_Open" in l), "FTL_Open [OK]"))
    except SystemExit:
        raise
    shutil.rmtree(td, ignore_errors=True)


if __name__ == "__main__":
    main()
