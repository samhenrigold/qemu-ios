#!/usr/bin/env python3
"""Boots n72ap-7E18 natively (run.sh), unlocks to Home once the lock screen lights, screendumps,
then pauses and saves the machine with QMP migrate to file: (as the helper's Stop does).
usage: save-home.py NAME QEMU-BINARY   ->  ~/Developer/wasm-spike-run/NAME/{state.bin,home.png}"""
import json, os, shutil, socket, subprocess, sys, time

name, binary = sys.argv[1], sys.argv[2]
here = os.path.dirname(os.path.abspath(__file__))
run = os.path.expanduser(f"~/Developer/wasm-spike-run/{name}")
sock = f"/tmp/wasmspike-{name}.qmp"
shutil.rmtree(run, ignore_errors=True)  # a previous run's stderr.log would read as lit
proc = subprocess.Popen([f"{here}/run.sh", name, "0", "--", binary, "--", "-qmp", f"unix:{sock},server=on,wait=off"])


def luma():
    try:
        lines = [l for l in open(f"{run}/stderr.log") if l.startswith("spike ")]
    except FileNotFoundError:
        return -1, 0
    return (float(lines[-1].split("luma=")[1]), len(lines)) if lines else (-1, 0)


while luma()[0] < 0.1:
    assert proc.poll() is None and luma()[1] < 120, "never lit"
    time.sleep(0.5)
time.sleep(2)

s = socket.socket(socket.AF_UNIX)
s.connect(sock)
f = s.makefile("rw")
f.readline()


def qmp(cmd, **args):
    f.write(json.dumps({"execute": cmd, "arguments": args}) + "\n")
    f.flush()
    while True:
        r = json.loads(f.readline())
        if "return" in r or "error" in r:
            return r


qmp("qmp_capabilities")


def touch(x, y, down=None):
    ev = [{"type": "abs", "data": {"axis": "x", "value": int(x * 32767 / 320)}},
          {"type": "abs", "data": {"axis": "y", "value": int(y * 32767 / 480)}}]
    if down is not None:
        ev.append({"type": "btn", "data": {"button": "left", "down": down}})
    qmp("input-send-event", events=ev)


# Slide to unlock: the 3.1.3 slider runs along the bottom bar, knob at the left.
touch(45, 430, True)
for i in range(1, 21):
    time.sleep(0.03)
    touch(45 + i * 12, 430)
touch(285, 430, False)
time.sleep(4)
qmp("screendump", filename=f"{run}/home.png", format="png")
print("home luma", luma()[0])
qmp("stop")
t = time.time()
print(qmp("migrate", uri=f"file:{run}/state.bin"))
while (st := qmp("query-migrate")["return"].get("status")) not in ("completed", "failed"):
    time.sleep(0.1)
print(f"save {st} in {time.time() - t:.2f} s, {os.path.getsize(run + '/state.bin') / 1e6:.0f} MB")
qmp("quit")
proc.wait()
