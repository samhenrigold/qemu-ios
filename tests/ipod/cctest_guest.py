#!/usr/bin/env python3
"""CommonCrypto known answers on the emulated iPod (contrib/it-cctest).

Builds it_cctest for armv6/3.1.3, boots a disposable overlay of --base-nand,
runs it through the guest agent and judges every line with the same answers
as tests/ipad1/cctest.py. Its AES cases over 64 blocks at 16-byte alignment
go through /dev/aes_0, i.e. the S5L8900 AES model (hw/arm/ipod_touch_aes.c);
buffers over a page take its segmented, interrupt-driven path.

    tests/ipod/cctest_guest.py [--qemu Q] [--base-nand DIR]
"""
import argparse, os, subprocess, sys, tempfile, time
from pathlib import Path
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/ipod"))
import regress as r, itqmp  # noqa: E402

ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
ap.add_argument("--files", default=str(ROOT.parent / "qemu-ios-files"))
ap.add_argument("--base-nand", default=None, help="default <files>/nand-current")
ap.add_argument("--qemu", default=str(ROOT / "build/qemu-system-arm"))
ap.add_argument("--timeout", type=int, default=300, help="seconds for the run itself")
a = ap.parse_args()

out = tempfile.mkdtemp(prefix="it-cctest-")
binary = os.path.join(out, "it_cctest")
env = dict(os.environ)
env.setdefault("ARMV6_SDK", str(ROOT.parent / "ipod2g-re/OldSDK/iPhoneOS3.1.3.sdk"))
subprocess.run(["bash", "-c", '. contrib/armv6-toolchain/armv6.sh && cc6 contrib/it-cctest/it_cctest.c "$1.o" '
                '&& link6 -execute "$1" "$1.o" && "${LDID:-ldid}" -S "$1"', "-", binary],
               cwd=ROOT, env=env, check=True)

f = a.files
cfg = SimpleNamespace(out=out, files=f, base_nand=os.path.realpath(a.base_nand or f + "/nand-current"),
                      nor=f + "/ios3/nor_7E18.bin", overlay=out + "/overlay", qemu=a.qemu,
                      usbmuxd_ok=False, usb_port=r.free_port(1540, 1559), mux_port=0,
                      qmp_port=r.free_port(28300, 28339), wifi=False, cpu=None, mem="128M",
                      kernel_console=False)
os.makedirs(cfg.overlay)
r.START = time.time()
procs = r.Procs()
log = b""
try:
    dev = r.Device(cfg, procs, "dev")
    dev.start()
    ok, detail, _ = dev.wait_for_home(600)
    if not ok:
        sys.exit("FAIL: no home screen (%s); %s" % (detail, out))
    deadline = time.monotonic() + 90
    while not itqmp.agent_alive(dev.qmp):
        if time.monotonic() > deadline:
            sys.exit("FAIL: guest agent never came up")
        time.sleep(1)
    with open(binary, "rb") as fh:
        itqmp.agent(dev.qmp, "put", "/tmp/it_cctest 755", fh.read())
    # The agent reaps its children when a request returns, so launchd runs it.
    itqmp.agent(dev.qmp, "exec", "launchctl submit -l com.qemu.it-cctest -o /tmp/cctest.txt "
                "-e /tmp/cctest.txt -- /tmp/it_cctest")
    deadline = time.monotonic() + a.timeout
    while b"it_cctest: done" not in log and time.monotonic() < deadline:
        time.sleep(10)
        log = itqmp.agent(dev.qmp, "get", "/tmp/cctest.txt")[1]
finally:
    procs.stop_all()
path = os.path.join(out, "cctest.txt")
with open(path, "wb") as fh:
    fh.write(log)
if b"it_cctest: done" not in log:
    sys.exit("FAIL: it_cctest did not finish in %d s (a hung /dev/aes_0 request?); %s" % (a.timeout, path))
sys.exit(subprocess.run([sys.executable, str(ROOT / "tests/ipad1/cctest.py"), path]).returncode)
