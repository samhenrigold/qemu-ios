#!/usr/bin/env python3
"""Boot the ipad1 machine for a while and report how far the 7B500 kernel got.

    tests/ipad1/boot-smoke.py [--seconds N] [--kboot PATH] [--qemu PATH] [--args "BOOT_ARGS"]

Exit 0 if the serial log reaches the furthest expected marker, 1 otherwise. Always
prints the last marker reached, the panic string if any, and the most-polled
unimplemented registers, so a regression or the next blocker is visible at a glance.
"""
import argparse, collections, os, re, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
FILES = os.path.expanduser("~/Developer/qemu-ios-files/ipad1/7B500")

# In boot order. The test passes when the last one appears.
MARKERS = [
    ("kernel", "iBoot version: iBoot-817.29"),
    ("platform", "AppleS5L8930XPerformanceController: Dynamic Performance State"),
    ("uart", "Identified Serial Port on ARM Device=uart0"),
    ("iop-start", "AppleS5L8920XARM7M::start: mapped I/O registers"),
    ("display", "AppleCLCD::start_hardware"),
    ("iop-firmware", "EmbeddedIOP firmware s5l8930x-RELEASE"),
    ("nand", "AppleS5L8920XIOPFMI"),
    ("ftl", "[FTL:MSG] FPart Init"),
    ("rootdev", "Waiting for root device"),
    ("bsd", "BSD root:"),
    ("launchd", "launchd"),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=int, default=45)
    ap.add_argument("--kboot", default=f"{FILES}/k48-kboot.bin")
    ap.add_argument("--qemu", default=f"{ROOT}/build/qemu-system-arm")
    ap.add_argument("--args", help="rebuild the bundle with these boot-args first")
    ap.add_argument("--nand", help="NAND page-store directory to attach (booted in place: it gets written)")
    ap.add_argument("--nand-clone", help="NAND store to APFS-clone into a temp dir and boot (the original is untouched)")
    a = ap.parse_args()

    if a.args:
        subprocess.run([sys.executable, f"{ROOT}/imgtools/ipad1_kboot.py",
                        f"{FILES}/dec", a.kboot, a.args], check=True)

    with tempfile.TemporaryDirectory() as td:
        serial, qlog = f"{td}/serial.log", f"{td}/qemu.log"
        if a.nand_clone:
            a.nand = f"{td}/nand"
            subprocess.run(["cp", "-cR", a.nand_clone, a.nand], check=True)  # APFS clone: instant, copy-on-write
            subprocess.run(["chmod", "-R", "u+w", a.nand], check=True)
        machine = f"ipad1,kboot={a.kboot}" + (f",nand={a.nand}" if a.nand else "")
        cmd = [a.qemu, "-machine", machine, "-display", "none",
               "-monitor", "none", "-serial", f"file:{serial}",
               "-d", "unimp,guest_errors", "-D", qlog]
        try:
            subprocess.run(cmd, timeout=a.seconds, stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL)
            ended = "qemu exited"
        except subprocess.TimeoutExpired:
            ended = f"still running after {a.seconds}s"
        text = open(serial, errors="replace").read() if os.path.exists(serial) else ""
        unimp = open(qlog, errors="replace").read() if os.path.exists(qlog) else ""

    reached = [name for name, needle in MARKERS if needle in text]
    last = reached[-1] if reached else "(nothing on serial)"
    panic = re.search(r"^panic\(.*$", text, re.M)
    offsets = collections.Counter(
        re.findall(r"offset (0x[0-9a-f]+)", unimp[-400000:]))

    print(f"{ended}; {len(text.splitlines())} serial lines; last marker: {last}")
    if panic:
        print("panic:", panic.group(0))
    if offsets:
        print("most-polled unimplemented offsets (peripheral window +0x80000000):")
        for off, n in offsets.most_common(8):
            print(f"  {n:6d}  {off}")
    if not reached and text:
        print("serial tail:\n" + "\n".join(text.splitlines()[-5:]))
    return 0 if last == MARKERS[-1][0] else 1


if __name__ == "__main__":
    sys.exit(main())
