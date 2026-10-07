#!/usr/bin/env python3
"""Audio out: boot a clone of a NAND store with -audio driver=wav and check that
the guest's system sounds reach the WAV intact.

    tests/ipad1/audio-check.py [--nand STORE] [--qemu PATH] [--keep DIR]

Boots STORE under a private overlay through the shared iPad boot harness
(the original NAND/NOR are never written), then:
  - the connect-power sound SpringBoard plays at boot  -> beep-beep.caf
  - Home, slide to unlock                               -> unlock.aiff
  - Hold with the display on                            -> lock.aiff
  - Home, slide to unlock again                         -> unlock.aiff
Each sound is found in the WAV by cross-correlation against the file from the
tested guest filesystem, played at 1.00x (waveform and timebase are checked).
The guest agent reads those resources; --sound-reference-root supplies an
explicit matching extracted rootfs for older images without an agent. Exit 0 when every one correlates >= MIN_CORR.
The lock/unlock pair covers the stop/restart path: before the I2S drained bit,
the first stop hung mediaserverd and every later sound was lost.
"""
import argparse, importlib.util, os, shutil, subprocess, sys, tempfile, time, wave

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "imgtools"))
import itqmp  # noqa: E402

FILES = os.path.expanduser("~/Developer/qemu-ios-files/ipad1")
ROOTFS = "/System/Library"
EXPECT = [
    ("boot: connect power", f"{ROOTFS}/Audio/UISounds/beep-beep.caf"),
    ("unlock", f"{ROOTFS}/CoreServices/SpringBoard.app/unlock.aiff"),
    ("lock (Hold)", f"{ROOTFS}/CoreServices/SpringBoard.app/lock.aiff"),
    ("unlock again", f"{ROOTFS}/CoreServices/SpringBoard.app/unlock.aiff"),
]
# 4.x SpringBoard plays UISounds/unlock.caf on unlock.
EXPECT_4 = [(n, f"{ROOTFS}/Audio/UISounds/unlock.caf" if n.startswith("unlock") else f) for n, f in EXPECT]
MIN_CORR = 0.8
LEVEL = 150          # |sample| above this is sound


def capture_references(q, destination, product_version="3.2.2", reference_root=None):
    """Read the tested guest's stock resources through its VFS, before shutdown.

    A legacy image without an agent needs an explicit matching rootfs directory;
    never silently compare another firmware's host-mounted resources.
    """
    os.makedirs(destination, exist_ok=True)
    expected = EXPECT_4 if product_version.startswith("4.") else EXPECT
    paths, result = {}, []
    for label, path in expected:
        if path not in paths:
            target = os.path.join(destination, os.path.basename(path))
            if reference_root:
                shutil.copyfile(os.path.join(reference_root, path.lstrip("/")), target)
            else:
                status, data = itqmp.agent(q, "get", path)
                if status or not data:
                    raise RuntimeError("cannot read stock sound %s from tested guest (%s); "
                                       "offer its guest agent or supply --sound-reference-root" % (path, status))
                with open(target, "wb") as f:
                    f.write(data)
            paths[path] = target
        result.append((label, paths[path]))
    return result


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


def judge(wav, serial=None, *, expect):
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


def play_sounds(q, start=(957, 480), end=(957, 67)):
    """Make stock SpringBoard play EXPECT[1:] (the boot sound comes by itself).

    q is an itqmp.QMP on a machine booted >= 40 s ago (lock screen up, boot
    sound done); takes about 25 s. start/end: the unlock slider's drag in scanout
    pixels of itqmp.W x itqmp.H (default the iPad's, 1024x768).
    """
    def unlock():
        itqmp.button(q, "home")               # wake the idle-dimmed lock screen
        time.sleep(2)
        itqmp.move(q, *start)                 # slide-to-unlock knob, scanout pixels
        q.cmd("input-send-event", events=[{"type": "btn", "data": {"down": True, "button": "left"}}])
        for i in range(1, 41):
            itqmp.move(q, start[0] + (end[0] - start[0]) * i // 40, start[1] + (end[1] - start[1]) * i // 40)
            time.sleep(0.03)
        time.sleep(0.3)   # rest at the end: 4.x reads a release while still moving as a flick back
        q.cmd("input-send-event", events=[{"type": "btn", "data": {"down": False, "button": "left"}}])
        time.sleep(6)

    unlock()
    itqmp.button(q, "power")
    time.sleep(4)
    unlock()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--nand", help="override selected device NAND")
    import ipad1_boot
    ipad1_boot.add_arguments(ap)
    ap.add_argument("--qemu", default=f"{ROOT}/build/qemu-system-arm")
    ap.add_argument("--keep", help="copy the WAV and serial log here")
    ap.add_argument("--sound-reference-root", help="explicit matching extracted rootfs for a guest without an agent")
    ap.add_argument("--product-version", help="sound selector (default: selected device lock, else 3.2.2)")
    ap.add_argument("--guest-package", help="current guest-agent offer for a legacy prepared image")
    a = ap.parse_args()
    itqmp.W, itqmp.H = 1024, 768
    a.nand = a.nand or os.path.join(a.device, "nand")
    spec = importlib.util.spec_from_file_location("audio_regress", os.path.join(HERE, "regress.py"))
    rg = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(rg)
    rg.device_args(a)
    rg.ipod.START = time.time()
    a.boot_timeout = 200
    with tempfile.TemporaryDirectory() as td:
        a.out = td
        wav = os.path.join(td, "out.wav")
        boot = rg.Boot(a, "audio", usb=False, wav=wav)
        serial = boot.serial
        try:
            boot.start()
            ok, detail = boot.wait_lock_screen()
            if not ok:
                raise RuntimeError(detail)
            time.sleep(15)
            expect = capture_references(boot.qmp, os.path.join(td, "references"),
                                        a.product_version, a.sound_reference_root)
            play_sounds(boot.qmp)
            boot.qmp.cmd("quit")
            boot.qemu.wait(timeout=30)
        finally:
            boot.stop()
        if a.keep:
            os.makedirs(a.keep, exist_ok=True)
            for f in (wav, serial):
                shutil.copy(f, a.keep)
            shutil.copytree(os.path.join(td, "references"), os.path.join(a.keep, "references"),
                            dirs_exist_ok=True)

        ok = judge(wav, serial, expect=expect)
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
