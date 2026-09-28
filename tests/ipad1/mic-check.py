#!/usr/bin/env python3
"""Audio in: the guest records an injected tone through the stock AudioQueue input path.

    tests/ipad1/mic-check.py [--hz 1000] [--keep DIR] [--qemu PATH]

Builds contrib/ipad1-mictest (it_mictest, ldid-signed), installs it with a
RunAtLoad launchd job into a scratch copy of the pristine system image (no
it_seal job, so the first boot is the test boot), makes a NAND store, then
boots it with -global driver=s5l8930.i2s,property=tone-hz,value=HZ, which
replaces the i2s0 capture input with a synthetic stereo sine. it_mictest
records 10 s (i2s0 RX -> CDMA ch 0x1b -> mediaserverd -> AudioQueue) and
prints "MICTEST frames=.. secs=.. rate=.. freq=.. peak=.. glitches=.." on the
serial console. PASS when freq is within 0.5% of HZ, rate within 2% of
44100 (buffers are 2048 frames, so a 10 s window quantises to about 0.5%),
and there are no discontinuities. About 4 minutes.
"""
import argparse, os, re, shutil, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "imgtools"))
import build_nand as bn        # noqa: E402
import ipad1_rootfs as rootfs  # noqa: E402

FILES = os.path.expanduser("~/Developer/qemu-ios-files/ipad1")
JOB = "System/Library/LaunchDaemons/com.qemu.mictest.plist"
PLIST = """<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>Label</key><string>com.qemu.mictest</string>
<key>ProgramArguments</key><array><string>/usr/local/bin/it_mictest</string></array>
<key>RunAtLoad</key><true/>
<key>StandardErrorPath</key><string>/dev/console</string>
</dict></plist>
"""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--hz", type=int, default=1000)
    ap.add_argument("--qemu", default=os.path.join(ROOT, "build/qemu-system-arm"))
    import ipad1_boot
    ipad1_boot.add_arguments(ap)   # carries the AMFI boot-args
    ap.add_argument("--base", help="override selected device NAND")
    ap.add_argument("--keep", help="keep the scratch store and serial log here")
    a = ap.parse_args()
    a.nand = a.nand or os.path.join(a.device, "nand")

    tool = os.path.join(ROOT, "build/ipad1-mictest/it_mictest")
    subprocess.run([os.path.join(ROOT, "contrib/ipad1-mictest/build.sh")], check=True,
                   stdout=subprocess.DEVNULL, timeout=300)
    work = a.keep or tempfile.mkdtemp(prefix="ipad1-mic-")
    os.makedirs(work, exist_ok=True)
    try:
        for img in ("system.img", "data.img"):
            subprocess.run(["cp", "-c", os.path.join(a.base, img), work], check=True)
        system = os.path.join(work, "system.img")
        with rootfs.Mounted(system, os.path.join(work, "mnt")) as m:
            dst = os.path.join(m.mnt, "usr/local/bin/it_mictest")
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copyfile(tool, dst)
            os.chmod(dst, 0o755)
            with open(os.path.join(m.mnt, JOB), "w") as f:
                f.write(PLIST)
            os.chmod(os.path.join(m.mnt, JOB), 0o644)
            seal = os.path.join(m.mnt, "System/Library/LaunchDaemons/com.qemu.it-seal.plist")
            if os.path.exists(seal):          # it halts the first boot of a baked image
                os.remove(seal)
        bn.set_owner(system, ["usr/local/bin/it_mictest", JOB], 0, 0)   # launchd wants root-owned jobs
        nand = os.path.join(work, "nand")
        shutil.rmtree(nand, ignore_errors=True)
        subprocess.run([sys.executable, os.path.join(ROOT, "imgtools/ipad1_nand.py"), "build",
                        "--mbr", f"{FILES}/hw2/rdisk0-head4M.bin", "--system", system,
                        "--data", os.path.join(work, "data.img"), "--out", nand],
                       check=True, stdout=subprocess.DEVNULL, timeout=500)
        serial = os.path.join(work, "serial.log")
        subprocess.run(["timeout", "150", a.qemu, "-machine",
                        f"ipad1,{ipad1_boot.boot_options(a)},nand={nand},nand-overlay={work}/overlay",
                        "-display", "none", "-monitor", "none", "-serial", f"file:{serial}",
                        "-audio", "driver=none",
                        "-global", f"driver=s5l8930.i2s,property=tone-hz,value={a.hz}"],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        log = open(serial, errors="replace").read()
    finally:
        if not a.keep:
            shutil.rmtree(work, ignore_errors=True)

    m = re.search(r"MICTEST frames=(\d+) secs=([\d.]+) rate=(\d+) freq=([\d.]+) peak=(\d+) glitches=(\d+)", log)
    for line in re.findall(r"^MICTEST.*$", log, re.M):
        print(line)
    if not m:
        print("FAIL: no MICTEST result on serial")
        return 1
    rate, freq, glitches = int(m[3]), float(m[4]), int(m[6])
    ok = abs(freq - a.hz) <= a.hz * 0.005 and abs(rate - 44100) <= 44100 * 0.02 and glitches == 0
    print(f"{'PASS' if ok else 'FAIL'}: tone {a.hz} Hz recorded as {freq} Hz, {rate} frames/s, "
          f"{glitches} discontinuities")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
