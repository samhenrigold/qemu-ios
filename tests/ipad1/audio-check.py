#!/usr/bin/env python3
"""Audio out: boot a clone of a NAND store with -audio driver=wav and check that
the guest's system sounds reach the WAV intact.

    tests/ipad1/audio-check.py [--nand STORE] [--qemu PATH] [--keep DIR]

Boots an APFS clone of STORE (default: the golden pristine store; the original
is never written), then:
  - the connect-power sound SpringBoard plays at boot  -> beep-beep.caf
  - Home, slide to unlock                               -> unlock.aiff
  - Hold with the display on                            -> lock.aiff
  - Home, slide to unlock again                         -> unlock.aiff
Each sound is found in the WAV by cross-correlation against the file from the
7B500 root filesystem, played at 1.00x (so rate, channel order and byte
alignment are all checked). Exit 0 when every one correlates >= MIN_CORR.
The lock/unlock pair covers the stop/restart path: before the I2S drained bit,
the first stop hung mediaserverd and every later sound was lost.
"""
import argparse, os, shutil, subprocess, sys, tempfile, time, wave

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "imgtools"))
import itqmp  # noqa: E402

itqmp.W, itqmp.H = 1024, 768
FILES = os.path.expanduser("~/Developer/qemu-ios-files/ipad1")
ROOTFS = f"{FILES}/7B500/mnt-rootfs/System/Library"
EXPECT = [
    ("boot: connect power", f"{ROOTFS}/Audio/UISounds/beep-beep.caf"),
    ("unlock", f"{ROOTFS}/CoreServices/SpringBoard.app/unlock.aiff"),
    ("lock (Hold)", f"{ROOTFS}/CoreServices/SpringBoard.app/lock.aiff"),
    ("unlock again", f"{ROOTFS}/CoreServices/SpringBoard.app/unlock.aiff"),
]
# 4.x SpringBoard plays UISounds/unlock.caf on unlock (the same file ships in 7B500's rootfs)
EXPECT_4 = [(n, f"{ROOTFS}/Audio/UISounds/unlock.caf" if n.startswith("unlock") else f) for n, f in EXPECT]
MIN_CORR = 0.8
LEVEL = 150          # |sample| above this is sound


def load(path):
    w = wave.open(path)
    a = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2")
    return a.reshape(-1, w.getnchannels()).astype(float)[:, 0], w.getframerate()


def reference(path, td):
    out = os.path.join(td, os.path.basename(path) + ".wav")
    subprocess.run(["afconvert", "-f", "WAVE", "-d", "LEI16@44100", "-c", "2", path, out],
                   check=True)
    a, _ = load(out)
    nz = np.nonzero(np.abs(a) > LEVEL)[0]
    return a[nz[0]:nz[-1] + 1]


def events(a, rate):
    """(start, end) sample spans of sound separated by >= 50 ms of quiet."""
    block, spans, start = rate // 20, [], None
    for i in range(0, len(a), block):
        loud = np.abs(a[i:i + block]).max() > LEVEL
        if loud and start is None:
            start = i
        elif not loud and start is not None:
            spans.append((start, i))
            start = None
    return spans


def corr(seg, ref):
    if len(ref) > len(seg):
        return 0.0
    c = np.correlate(seg, ref, "valid")
    k = int(np.argmax(np.abs(c)))
    s = seg[k:k + len(ref)]
    return float(np.dot(s, ref) / (np.linalg.norm(s) * np.linalg.norm(ref) + 1e-9))


def judge(wav, serial=None, expect=EXPECT):
    """Check a WAV against `expect` [(label, rootfs sound file), ...] in order.

    Prints one line per sound; returns True when the WAV is 44.1 kHz, holds at
    least len(expect) sounds and each correlates >= MIN_CORR with its file at
    1.00x, and (if given) the serial log has no kernel panic.
    """
    with tempfile.TemporaryDirectory() as td:
        cap, rate = load(wav)
        spans = events(cap, rate)
        ok = rate == 44100 and len(spans) >= len(expect)
        print(f"WAV {rate} Hz, {len(cap) / rate:.1f} s of active output, {len(spans)} sounds")
        next_span = 0
        for name, path in expect:
            ref = reference(path, td)
            best, found = 0.0, None
            # SpringBoard may emit additional sounds during a drag. Require
            # each expected waveform, in order, without treating every sound
            # boundary as the next test action. Never reuse a matched event.
            for i in range(next_span, len(spans)):
                lo, hi = spans[i]
                c = corr(cap[max(0, lo - rate // 10):hi + rate // 10], ref)
                best = max(best, c)
                if c >= MIN_CORR:
                    found = i
                    break
            ok &= found is not None
            if found is not None:
                next_span = found + 1
            print(f"  {'ok  ' if found is not None else 'FAIL'} {name:22s} {os.path.basename(path):14s} corr {best:.2f}")
        if len(spans) < len(expect):
            print(f"  FAIL expected {len(expect)} sounds")
    if serial and "panic(" in open(serial, errors="replace").read():
        print("  FAIL kernel panic on serial")
        ok = False
    return ok


def play_sounds(q):
    """Make stock SpringBoard play EXPECT[1:] (the boot sound comes by itself).

    q is an itqmp.QMP on a machine booted >= 40 s ago (lock screen up, boot
    sound done); takes about 25 s. itqmp.W/H must be 1024x768.
    """
    def unlock():
        itqmp.button(q, "home")               # wake the idle-dimmed lock screen
        time.sleep(2)
        itqmp.move(q, 957, 480)               # slide-to-unlock knob, scanout pixels (upright portrait)
        q.cmd("input-send-event", events=[{"type": "btn", "data": {"down": True, "button": "left"}}])
        for i in range(1, 41):
            itqmp.move(q, 957, 480 - (480 - 67) * i // 40)
            time.sleep(0.03)
        time.sleep(0.3)   # rest at the end: 4.x reads a release while still moving as a flick back
        q.cmd("input-send-event", events=[{"type": "btn", "data": {"down": False, "button": "left"}}])
        time.sleep(6)

    unlock()
    itqmp.button(q, "power")
    time.sleep(4)
    unlock()


def drive(sock, qemu):
    while not os.path.exists(sock):
        if qemu.poll() is not None:
            raise SystemExit("qemu exited before QMP came up")
        time.sleep(0.2)
    q = itqmp.QMP(sock)
    time.sleep(40)                            # SpringBoard up, boot sound done
    play_sounds(q)
    q.cmd("quit")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--nand", help="override selected device NAND")
    import ipad1_boot
    ipad1_boot.add_arguments(ap)
    ap.add_argument("--qemu", default=f"{ROOT}/build/qemu-system-arm")
    ap.add_argument("--keep", help="copy the WAV and serial log here")
    a = ap.parse_args()
    a.nand = a.nand or os.path.join(a.device, "nand")

    with tempfile.TemporaryDirectory() as td:
        nand = os.path.join(td, "nand")
        subprocess.run(["cp", "-c", "-R", a.nand, nand], check=True)      # APFS clone
        subprocess.run(["chmod", "-R", "u+w", nand], check=True)
        wav, serial = os.path.join(td, "out.wav"), os.path.join(td, "serial.log")
        sock = f"/tmp/ipad1-audio-{os.getpid()}.qmp"                      # sun_path < 104
        qemu = subprocess.Popen(
            ["timeout", "200", a.qemu, "-machine", f"ipad1,{ipad1_boot.boot_options(a)},nand={nand}",
             "-display", "none", "-monitor", "none", "-serial", f"file:{serial}",
             "-qmp", f"unix:{sock},server,nowait", "-audio", f"driver=wav,path={wav}"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            drive(sock, qemu)
        finally:
            try:
                qemu.wait(timeout=30)
            except subprocess.TimeoutExpired:
                qemu.kill()
            if os.path.exists(sock):
                os.unlink(sock)
        if a.keep:
            os.makedirs(a.keep, exist_ok=True)
            for f in (wav, serial):
                shutil.copy(f, a.keep)

        ok = judge(wav, serial)
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
