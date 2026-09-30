#!/usr/bin/env python3
"""The coreaudio backend must play the guest at the OUTPUT DEVICE's rate.

    tests/ipod/test_coreaudio_rate.py [--qemu build/qemu-system-arm-unsigned] [--keep DIR]

Issue #13: on AirPods the emulator's sounds were higher-pitched and crackly.
AirPods run 48 kHz (24 or 16 kHz with the mic open) and cannot run 44.1 kHz;
the backend handed the device 44.1 kHz frames anyway, so they played 8.8%
fast and the device drained them faster than the guest made them.

Each case boots the iPod touch machine on a spin-loop bootrom (no firmware, no
NAND) with IT_I2S_TONE: a 1 kHz sine at the guest's 44.1 kHz goes into the I2S
TX FIFO and down the same path guest PCM takes (I2S ring -> AUD_write ->
mixeng -> coreaudio IOProc). The output device is tests/ipod/coreaudio-fakehal.c,
interposed with DYLD_INSERT_LIBRARIES: a fixed-rate device on its own clock that
records what it is handed. Nothing reaches the speakers and the Mac's default
output is never touched. Per recorded segment (at that segment's device rate):

  - pitch: the tone measures 1000 Hz within 1%
  - clicks: no dropout (>= 4 consecutive exact zeros) and no sample-to-sample
    step larger than a clean 1 kHz sine at that rate can make, anywhere in the
    tone except 150 ms around a device switch (the old device's last buffers).

Cases: 16k, 24k, 44.1k and 48k devices; 48k -> 24k on the same device (AirPods
going to HFP); a 48k device replaced by a 16k default device (AirPods
connecting). Needs the unsigned build (the signed one's runtime drops
DYLD_INSERT_LIBRARIES). One emulator at a time, ~7 s each.
"""
import argparse, json, os, signal, subprocess, sys, tempfile, time

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TONE_HZ, TONE_SECS, AMP = 1000.0, 4.0, 12000 / 32768
CASES = [("16k", "16000"), ("24k", "24000"), ("44.1k", "44100"), ("48k", "48000"),
         ("48k->24k same device", "48000,24000@2"),
         ("48k->16k new default device", "48000,16000@2:dev")]


def run(qemu, dylib, plan, td):
    out = os.path.join(td, plan.replace(",", "_").replace("@", "at").replace(":", "") + ".f32")
    rom = os.path.join(td, "spin.bin")
    with open(rom, "wb") as f:
        f.write(b"\xfe\xff\xff\xea")                 # b . (the CPU just spins)
    env = dict(os.environ, FAKEHAL_PLAN=plan, FAKEHAL_OUT=out, DYLD_INSERT_LIBRARIES=dylib,
               IT_I2S_TONE=str(TONE_SECS), IT_I2S_TONE_HZ=str(TONE_HZ))
    p = subprocess.Popen([qemu, "-M", f"iPod-Touch,bootrom={rom}", "-display", "none",
                          "-audio", "driver=coreaudio,out.buffer-count=16"],
                         env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    time.sleep(TONE_SECS + 2.5)
    p.send_signal(signal.SIGINT)
    err = p.communicate(timeout=20)[1].decode(errors="replace")
    meta = json.load(open(out + ".json"))
    a = np.fromfile(out, dtype="<f4").reshape(-1, meta["channels"])[:, 0]
    return a, meta["segments"], err


def check_segment(x, rate, lo, hi):
    """Problems in x[lo:hi] (the tone, played at `rate`)."""
    probs = []
    loud = np.nonzero(np.abs(x[lo:hi]) > AMP / 4)[0]
    if len(loud) < rate // 10:
        return [f"no tone ({len(loud)} loud samples)"]
    s, e = lo + loud[0] + rate // 50, lo + loud[-1] - rate // 50   # skip onset/offset 20 ms
    t = x[s:e]
    zc = np.nonzero((t[:-1] < 0) & (t[1:] >= 0))[0]
    frac = zc + t[zc] / (t[zc] - t[zc + 1])
    hz = (len(frac) - 1) * rate / (frac[-1] - frac[0])
    if abs(hz - TONE_HZ) > TONE_HZ * 0.01:
        probs.append(f"pitch {hz:.1f} Hz (want {TONE_HZ:.0f} +-1%)")
    z = (t == 0).astype(np.int8)
    runs = np.diff(np.concatenate(([0], z, [0])))
    lens = np.nonzero(runs == -1)[0] - np.nonzero(runs == 1)[0]
    if (lens >= 4).any():
        probs.append(f"{int((lens >= 4).sum())} dropouts (longest {lens.max()} samples)")
    step = np.abs(np.diff(t)).max()
    limit = 1.25 * AMP * 2 * np.sin(np.pi * TONE_HZ / rate) + 1e-3
    if step > limit:
        probs.append(f"step {step:.3f} > {limit:.3f} (click)")
    return probs, hz


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qemu", default=os.path.join(ROOT, "build/qemu-system-arm-unsigned"))
    ap.add_argument("--keep")
    args = ap.parse_args()
    td = args.keep or tempfile.mkdtemp(prefix="ca-rate.")
    os.makedirs(td, exist_ok=True)
    dylib = os.path.join(td, "fakehal.dylib")
    subprocess.run(["clang", "-O2", "-dynamiclib", "-framework", "CoreAudio", "-framework",
                    "CoreFoundation", os.path.join(ROOT, "tests/ipod/coreaudio-fakehal.c"),
                    "-o", dylib], check=True)
    failed = 0
    for name, plan in CASES:
        a, segs, err = run(args.qemu, dylib, plan, td)
        bounds = [f for f, _ in segs] + [len(a)]
        probs = []
        if not segs:
            probs.append("device recorded nothing")
        for i, (f0, rate) in enumerate(segs):
            lo = f0 + (int(0.15 * rate) if i else 0)
            hi = bounds[i + 1] - (int(0.15 * rate) if i + 1 < len(segs) else 0)
            r = check_segment(a, int(rate), lo, hi)
            if isinstance(r, list):
                probs += [f"@{rate:.0f}: {p}" for p in r]
            else:
                p, hz = r
                probs += [f"@{rate:.0f}: {q}" for q in p]
                print(f"      segment @{rate:.0f} Hz: tone {hz:.1f} Hz")
        print(f"{'FAIL' if probs else 'PASS'}  {name}" + "".join(f"\n      {p}" for p in probs))
        failed += bool(probs)
    if not args.keep:
        subprocess.run(["rm", "-rf", td])
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
