#!/usr/bin/env python3
"""Boot the ipad1 machine for a while and report how far the 7B500 kernel got.

    tests/ipad1/boot-smoke.py [--seconds N] [--kboot PATH] [--qemu PATH] [--args "BOOT_ARGS"]
                              [--nand DIR | --nand-clone DIR | --nand-overlay BASE]
                              [--checkpoint-out DIR] [--from-checkpoint DIR]

Exit 0 if the serial log reaches the furthest expected marker, 1 otherwise. Always
prints the last marker reached with the wall time it first appeared, the panic string
if any, and the most-polled unimplemented registers, so a regression or the next
blocker is visible at a glance. The run stops early once the last marker (or, with a
checkpoint option, the lit lock screen) has been seen.

Checkpoints (live state + the NAND overlay, restored in seconds instead of a ~4 min boot):
  --checkpoint-out DIR   boot BASE (default golden-pristine) read-only with a fresh
                         overlay, wait for the lock screen, then save DIR/state (QEMU
                         migration stream) and DIR/overlay (APFS clone of the overlay).
  --from-checkpoint DIR  restore DIR onto a clone of its overlay, check the screen is
                         lit, and (with --keep) leave QEMU running for scripting through
                         the printed QMP socket.
"""
import argparse, collections, json, os, re, shutil, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
FILES = os.path.expanduser("~/Developer/qemu-ios-files/ipad1/7B500")
GOLDEN = os.path.expanduser("~/Developer/qemu-ios-files/ipad1/userland/golden-pristine")
sys.path.insert(0, f"{ROOT}/imgtools")

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
LIT = 20000      # lit samples in a screendump: the boot logo is well under, the lock screen far over


def lit(q, path):
    """Count bright bytes in a subsampled screendump (0 while the panel is dark)."""
    q.cmd("screendump", filename=path)
    for _ in range(40):
        if os.path.exists(path) and os.path.getsize(path) > 1000:
            break
        time.sleep(0.05)
    data = open(path, "rb").read()
    os.unlink(path)
    return sum(1 for b in data[len(data) // 20::13] if b > 60)


def wait_migration(q, deadline):
    while True:
        st = q.cmd("query-migrate")
        if st.get("status") == "completed":
            return
        if st.get("status") == "failed" or time.monotonic() > deadline:
            raise SystemExit(f"migration failed: {st}")
        time.sleep(0.1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=int, default=45)
    ap.add_argument("--kboot", default=f"{FILES}/k48-kboot.bin")
    ap.add_argument("--qemu", default=f"{ROOT}/build/qemu-system-arm")
    ap.add_argument("--args", help="rebuild the bundle with these boot-args first")
    ap.add_argument("--nand", help="NAND page-store directory to attach (booted in place: it gets written)")
    ap.add_argument("--nand-clone", help="NAND store to APFS-clone into a temp dir and boot (the original is untouched)")
    ap.add_argument("--nand-overlay", metavar="BASE", help="boot BASE read-only with a fresh copy-on-write overlay")
    ap.add_argument("--checkpoint-out", metavar="DIR", help="save a lock-screen checkpoint to DIR")
    ap.add_argument("--from-checkpoint", metavar="DIR", help="restore the checkpoint in DIR")
    ap.add_argument("--keep", action="store_true", help="with --from-checkpoint: leave QEMU running")
    a = ap.parse_args()

    if a.args:
        subprocess.run([sys.executable, f"{ROOT}/imgtools/ipad1_kboot.py",
                        f"{FILES}/dec", a.kboot, a.args], check=True)
    meta = {}
    if a.from_checkpoint:
        meta = json.load(open(f"{a.from_checkpoint}/checkpoint.json"))
        a.kboot, a.nand_overlay = meta["kboot"], meta["base"]
    elif a.checkpoint_out:
        a.nand_overlay = a.nand_overlay or GOLDEN

    td = tempfile.mkdtemp(prefix="ipad1-", dir="/tmp")      # short: unix socket paths cap at 104 bytes
    serial, qlog, qmp_path = f"{td}/serial.log", f"{td}/qemu.log", f"{td}/qmp"
    if a.nand_clone:
        a.nand = f"{td}/nand"
        subprocess.run(["cp", "-cR", a.nand_clone, a.nand], check=True)  # APFS clone: instant, copy-on-write
        subprocess.run(["chmod", "-R", "u+w", a.nand], check=True)
    machine = f"ipad1,kboot={a.kboot}"
    if a.nand_overlay:
        overlay = f"{td}/overlay"
        if a.from_checkpoint:
            subprocess.run(["cp", "-cR", f"{a.from_checkpoint}/overlay", overlay], check=True)
        else:
            os.mkdir(overlay)
        machine += f",nand={a.nand_overlay},nand-overlay={overlay}"
    elif a.nand:
        machine += f",nand={a.nand}"
    cmd = [a.qemu, "-machine", machine, "-display", "none", "-monitor", "none",
           "-qmp", f"unix:{qmp_path},server=on,wait=off", "-serial", f"file:{serial}",
           "-d", "unimp,guest_errors", "-D", qlog]
    if a.from_checkpoint:
        cmd += ["-incoming", f"file:{a.from_checkpoint}/state"]

    from itqmp import QMP
    t0 = time.monotonic()
    child = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=open(f"{td}/stderr", "w"),
                             start_new_session=a.keep)     # --keep: outlive this script's process group
    q, seen, text, screen_at, ended = None, {}, "", None, None
    want_screen = bool(a.checkpoint_out or a.from_checkpoint)
    try:
        while not os.path.exists(qmp_path):
            if child.poll() is not None:
                break
            time.sleep(0.05)
        if child.poll() is None:
            q = QMP(qmp_path, timeout=30)
        if a.from_checkpoint and q:
            while q.cmd("query-status")["status"] in ("inmigrate", "postmigrate"):
                time.sleep(0.05)
            q.cmd("cont")
            print(f"restored in {time.monotonic() - t0:.1f}s")
        next_shot = 0
        while child.poll() is None:
            t = time.monotonic() - t0
            if t > a.seconds:
                ended = f"still running after {a.seconds}s"
                break
            if os.path.exists(serial):
                text = open(serial, errors="replace").read()
            for name, needle in MARKERS:
                if name not in seen and needle in text:
                    seen[name] = t
            if want_screen and t >= next_shot:
                next_shot = t + 3
                if lit(q, f"{td}/s.ppm") > LIT:
                    screen_at = t
                    break
            elif not want_screen and MARKERS[-1][0] in seen:
                break
            time.sleep(0.25)
        ended = ended or ("qemu exited" if child.poll() is not None else "stopped early")

        if a.checkpoint_out and screen_at is not None:
            time.sleep(3)           # settle, but freeze well before the lock screen blanks (~8 s)
            q.cmd("stop")
            os.makedirs(a.checkpoint_out, exist_ok=True)
            state = f"{a.checkpoint_out}/state"
            if os.path.exists(state):
                os.unlink(state)
            q.cmd("migrate", uri=f"file:{state}")
            wait_migration(q, time.monotonic() + 120)
            dst = f"{a.checkpoint_out}/overlay"
            shutil.rmtree(dst, ignore_errors=True)
            subprocess.run(["cp", "-cR", overlay, dst], check=True)   # CPUs stopped: the mmaps are quiescent
            json.dump({"kboot": os.path.abspath(a.kboot), "base": os.path.abspath(a.nand_overlay),
                       "qemu": os.path.abspath(a.qemu), "saved": time.strftime("%Y-%m-%d %H:%M:%S")},
                      open(f"{a.checkpoint_out}/checkpoint.json", "w"), indent=1)
            print(f"checkpoint saved to {a.checkpoint_out} ({os.path.getsize(state) >> 20} MiB state)")
        if a.from_checkpoint and a.keep and screen_at is not None:
            print(f"QEMU pid {child.pid} left running; QMP at {qmp_path}")
            q.close()
            return 0
    finally:
        if child.poll() is None and not (a.keep and a.from_checkpoint and screen_at is not None):
            try:
                q and q.cmd("quit")
                child.wait(timeout=10)
            except Exception:
                child.kill()
                child.wait()
    unimp = open(qlog, errors="replace").read() if os.path.exists(qlog) else ""
    if not a.keep:
        shutil.rmtree(td, ignore_errors=True)

    reached = [name for name, _ in MARKERS if name in seen]
    last = reached[-1] if reached else "(nothing on serial)"
    panic = re.search(r"^panic\(.*$", text, re.M)
    offsets = collections.Counter(re.findall(r"offset (0x[0-9a-f]+)", unimp[-400000:]))

    print(f"{ended}; {len(text.splitlines())} serial lines; last marker: {last}")
    print("  " + "  ".join(f"{n}@{seen[n]:.1f}s" for n in reached))
    if want_screen:
        print(f"  screen lit: {f'{screen_at:.1f}s' if screen_at is not None else 'never'}")
    if panic:
        print("panic:", panic.group(0))
    if offsets:
        print("most-polled unimplemented offsets (peripheral window +0x80000000):")
        for off, n in offsets.most_common(8):
            print(f"  {n:6d}  {off}")
    if want_screen:
        return 0 if screen_at is not None else 1
    return 0 if last == MARKERS[-1][0] else 1


if __name__ == "__main__":
    sys.exit(main())
