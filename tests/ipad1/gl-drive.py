#!/usr/bin/env python3
"""Boot the ipad1 machine on a NAND overlay and run a scripted touch session.

    tests/ipad1/gl-drive.py --nand STORE --out DIR [--kboot K] [--seconds N] STEP...

STEP is one of  sleep:S  shot:NAME  tap:X,Y  swipe:X1,Y1,X2,Y2  home  wait:TEXT
(scanout pixels, 1024x768; wait:TEXT polls the serial log). The base store is
never written: its changes go to DIR/overlay. Serial, QEMU stderr (the GLES
host log) and NAME.png screendumps land in DIR.
"""
import argparse, os, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "imgtools"))
import itqmp  # noqa: E402

itqmp.W, itqmp.H = 1024, 768
FILES = os.path.expanduser("~/Developer/qemu-ios-files/ipad1")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--nand", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--kboot", default=f"{FILES}/userland/gl/k48-kboot-amfi.bin")
    ap.add_argument("--qemu", default=f"{ROOT}/build/qemu-system-arm")
    ap.add_argument("--seconds", type=int, default=900)
    ap.add_argument("--qemu-arg", action="append", default=[], help="extra QEMU argument (repeatable)")
    ap.add_argument("steps", nargs="*")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    sock, serial = f"/tmp/ipad1-gl-{os.getpid()}.qmp", f"{a.out}/serial.log"   # sun_path < 104
    for p in (sock,):
        if os.path.exists(p):
            os.unlink(p)
    machine = f"ipad1,kboot={a.kboot},nand={a.nand},nand-overlay={a.out}/overlay"
    qemu = subprocess.Popen(["timeout", str(a.seconds), a.qemu, "-machine", machine,
                             "-display", "none", "-monitor", "none",
                             "-serial", f"file:{serial}", "-qmp", f"unix:{sock},server,nowait"] + a.qemu_arg,
                            stdout=subprocess.DEVNULL, stderr=open(f"{a.out}/qemu.log", "w"))
    t0 = time.time()
    try:
        while not os.path.exists(sock):
            if qemu.poll() is not None:
                raise SystemExit(open(f"{a.out}/qemu.log").read())
            time.sleep(0.2)
        q = itqmp.QMP(sock)
        for step in a.steps:
            op, _, arg = step.partition(":")
            nums = [int(v) for v in arg.split(",")] if op in ("tap", "swipe") else []
            print("[%4.0fs] %s" % (time.time() - t0, step), flush=True)
            if op == "sleep":
                time.sleep(float(arg))
            elif op == "wait":
                while arg not in open(serial, errors="replace").read():
                    if qemu.poll() is not None:
                        raise SystemExit("qemu exited while waiting for %r" % arg)
                    time.sleep(1)
            elif op == "shot":
                q.cmd("screendump", filename=f"{a.out}/{arg}.ppm")
                time.sleep(0.5)
                subprocess.run(["sips", "-s", "format", "png", f"{a.out}/{arg}.ppm", "--out",
                                f"{a.out}/{arg}.png"], stdout=subprocess.DEVNULL, check=True)
                os.unlink(f"{a.out}/{arg}.ppm")
            elif op == "tap":
                itqmp.tap(q, *nums)
            elif op == "swipe":
                itqmp.swipe(q, *nums, steps=20, dt=0.03)
            elif op == "home":
                itqmp.button(q, "home")
            else:
                raise SystemExit("bad step %r" % step)
        q.cmd("quit")
    finally:
        try:
            qemu.wait(timeout=20)
        except subprocess.TimeoutExpired:
            qemu.kill()
        if os.path.exists(sock):
            os.unlink(sock)
    print("done after %.0fs" % (time.time() - t0))


if __name__ == "__main__":
    main()
