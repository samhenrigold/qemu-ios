#!/usr/bin/env python3
"""Boot 3.1.3 and screendump every ~1.2 s from t=0, to catch the boot-logo stage."""
import json, os, socket, subprocess, sys, time

S = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, "/Users/shg/Developer/qemu-ios/.claude/worktrees/consolidate/imgtools")
import itdrive as it

PORT = int(sys.argv[1])
TAG = sys.argv[2]
DUR = float(sys.argv[3]) if len(sys.argv) > 3 else 70
F = os.path.expanduser("~/Developer/qemu-ios-files")
HERE = F + "/ios3"
QEMU = "/Users/shg/Developer/qemu-ios/.claude/worktrees/consolidate/build/qemu-system-arm"
NAND = os.environ.get("NAND", F + "/nand-7e18-final")
OUT = "%s/boot_%s" % (S, TAG)
OVL = OUT + "/nandrw"
os.makedirs(OVL, exist_ok=True)
if os.environ.get("SEED_OVL"):
    __import__("subprocess").call("cp -R %s/. %s" % (os.environ["SEED_OVL"], OVL), shell=True)

env = dict(os.environ)
env.setdefault("IT_LCD_BRIGHT", "255")
env.update(IT_DIRECT_IBOOT=HERE + "/iBoot.bin",
           IT_INJECT_DT=HERE + "/DeviceTree.nowdt.bin",
           IT_WDT_NORESET="1", IT_TVOUT_READY="1")
for k, v in list(os.environ.items()):
    if k.startswith("IT_SET_"):
        env[k[len("IT_SET_"):]] = v
    if k.startswith("IT_UNSET_"):
        env.pop(v, None)

cmd = [QEMU, "-M",
       "iPod-Touch,bootrom=%s/bootrom_240_4,nand=%s,nor=%s,nandrw=%s"
       % (F, NAND, os.environ.get("NOR", HERE + "/nor_7E18.bin"), OVL),
       "-cpu", "max", "-m", "2G", "-display", "none",
       "-serial", "file:" + OUT + "/serial.log",
       "-qmp", "tcp:127.0.0.1:%d,server=on,wait=off" % PORT]
log = open(OUT + "/qemu.log", "wb")
t0 = time.time()
p = subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT, env=env)
q = it.QMP("127.0.0.1", PORT)
print("qmp up at t+%.1fs (pid %d)" % (time.time() - t0, p.pid))

rows = []
n = 0
while time.time() - t0 < DUR:
    if p.poll() is not None:
        print("QEMU EXITED rc=%s at t+%.1f" % (p.returncode, time.time() - t0))
        break
    ppm = "%s/f%03d.ppm" % (OUT, n)
    t = time.time() - t0
    try:
        q.cmd("screendump", filename=ppm)
    except Exception as e:
        print("screendump failed at t+%.1f: %r" % (t, e))
        break
    for _ in range(30):
        if os.path.exists(ppm) and os.path.getsize(ppm) > 1000:
            break
        time.sleep(0.05)
    try:
        d = open(ppm, "rb").read()
        i = d.index(b"255\n") + 4
        pix = d[i:]
        hi = max(pix) if pix else 0
        nz = sum(1 for v in pix if v)
        rows.append((n, t, hi, nz, len(pix)))
        print("f%03d t+%5.1fs max=%3d nonzero=%7d (%.2f%%)"
              % (n, t, hi, nz, 100.0 * nz / max(1, len(pix))))
    except Exception as e:
        print("f%03d t+%5.1fs parse fail %r" % (n, t, e))
    n += 1
    time.sleep(float(os.environ.get('SHOT_INTERVAL','1.2')))

json.dump(rows, open(OUT + "/frames.json", "w"))
print("done; leaving qemu pid %d running" % p.pid)
open(OUT + "/qemu.pid", "w").write(str(p.pid))
