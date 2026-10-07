#!/usr/bin/env python3
"""The iPhone (M68) plays its system sounds: Hold on the home screen plays SpringBoard's lock.aiff.

    tests/ipod/test_m68_audio.py --device DEV --reference-root ROOTFS [--qemu build/qemu-system-arm] [--keep DIR]

DEV is a FirmwareKit m68ap device (iBoot.bin, nand/, nor.bin, device.lock.json); ROOTFS the same build's
mounted root filesystem (for System/Library/CoreServices/SpringBoard.app/lock.aiff). The device boots under
a private overlay with -audio driver=wav, is unlocked, and Hold locks it; the WAV must hold the lock sound,
correlating >= 0.8 with the file at 1.00x (tests/ipad1/audio-check.py's judge).

Fails with no sound at all when the baseband's I2S port (0x3CD00000) is plain RAM: AppleBasebandOutput's DMA
start never sees its ready interrupt and mediaserverd's StartIO fails for every sound. Only one sound: the
1G's wake from sleep is not modeled (docs/ipod1g debt 9), so the device stays asleep after it.
"""
import argparse, importlib.util, json, os, shutil, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "imgtools"))
import itqmp  # noqa: E402

spec = importlib.util.spec_from_file_location("audio_check", os.path.join(ROOT, "tests/ipad1/audio-check.py"))
ac = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ac)
BOOTROM = os.path.expanduser("~/Developer/qemu-ios-files/ipod1g/bootrom_s5l8900")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--device", required=True)
    ap.add_argument("--reference-root", required=True)
    ap.add_argument("--qemu", default=os.path.join(ROOT, "build/qemu-system-arm"))
    ap.add_argument("--keep")
    a = ap.parse_args()
    lock_sound = os.path.join(a.reference_root, "System/Library/CoreServices/SpringBoard.app/lock.aiff")
    machine = json.load(open(os.path.join(a.device, "device.lock.json"))).get("machine", {})
    work = a.keep or tempfile.mkdtemp(prefix="m68-audio-")
    os.makedirs(os.path.join(work, "ovl"), exist_ok=True)
    nor = os.path.join(work, "nor.bin")
    shutil.copy(os.path.join(a.device, "nor.bin"), nor)
    os.chmod(nor, 0o600)
    sock, wav = os.path.join(work, "q.sock"), os.path.join(work, "out.wav")
    m = "iPhone-2G,bootrom=%s,iboot=%s/iBoot.bin,nand=%s/nand,nand-overlay=%s/ovl" % (BOOTROM, a.device, a.device, work)
    m += "".join(",%s=%s" % (k, machine[k]) for k in ("imei", "wifi-mac") if k in machine)
    q = None
    p = subprocess.Popen([a.qemu, "-M", m, "-drive", "if=pflash,format=raw,file=" + nor,
                          "-serial", "file:" + os.path.join(work, "serial.log"), "-display", "none",
                          "-audio", "driver=wav,path=" + wav, "-qmp", "unix:%s,server,nowait" % sock],
                         stdout=open(os.path.join(work, "qemu.log"), "w"), stderr=subprocess.STDOUT)
    try:
        time.sleep(2)
        q = itqmp.QMP(sock, timeout=60)
        t0 = time.time()
        while time.time() - t0 < 300:     # SpringBoard is up ~100 s of guest time in
            time.sleep(5)
            w, h, pix = itqmp.read_ppm(itqmp.shot_ppm(q, os.path.join(work, "s.ppm")))
            if time.time() - t0 > 60 and sum(1 for v in pix[::7] if v > 40) / len(pix[::7]) > 0.2:
                break
        time.sleep(20)
        itqmp.swipe(q, 40, 420, 300, 420, steps=20)     # slide to unlock (if it is up)
        time.sleep(4)
        itqmp.button(q, "power", hold_ms=400)            # Hold: the lock sound
        time.sleep(4)
        q.cmd("quit")
        p.wait(timeout=30)                               # the WAV header is written at exit
    finally:
        if p.poll() is None:
            p.kill()
            p.wait()
    ok = os.path.getsize(wav) > 44 and ac.judge(wav, os.path.join(work, "serial.log"), expect=[("lock (Hold)", lock_sound)])
    print("PASS: the lock sound reached the WAV" if ok else "FAIL: no lock sound in the WAV (see %s)" % work)
    if ok and not a.keep:
        shutil.rmtree(work, ignore_errors=True)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
