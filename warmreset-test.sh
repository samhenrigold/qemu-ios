#!/bin/bash
# Does the display come back after a warm reset?
#   warmreset-test.sh <tag>
# Boot to the home screen, screenshot, system_reset over QMP, wait for the second
# boot, screenshot again. No dirty-overlay setup needed -- a bare system_reset
# exercises exactly the same path the guest's post-fsck reboot uses.
set -u
TAG="$1"
D="/private/tmp/claude-501/-Users-shg-Developer-qemu-ios/ebb367f9-df80-473b-a8c5-d32a64cc0728/scratchpad"
F="$HOME/Developer/qemu-ios-files"
Q="$HOME/Developer/wt-nand/build/arm-softmmu/qemu-system-arm"
T="$HOME/Developer/wt-nand/qmp-touch.py"
PORT=4521

"$Q" -M iPod-Touch,bootrom="$F/bootrom_240_4",nand="$F/nand",nor="$F/nor_n72ap.bin" \
  -serial file:"$D/$TAG.serial" -cpu max -m 2G -display none \
  -qmp tcp:127.0.0.1:$PORT,server,nowait > "$D/$TAG.stdout" 2>&1 &
PID=$!
echo "pid=$PID"

shot() {  # shot <name>
  python3 "$T" $PORT shot "$D/$TAG-$1.ppm" >/dev/null 2>&1 || true
  python3 - "$D/$TAG-$1.ppm" "$1" <<'EOF'
import sys
try:
    f = open(sys.argv[1], 'rb'); f.readline()
    l = f.readline()
    while l.startswith(b'#'): l = f.readline()
    w, h = map(int, l.split()); f.readline()
    d = bytearray(f.read())
    lit = sum(1 for v in d if v > 8)
    print("  %-10s maxpx=%3d lit_bytes=%d %s" % (sys.argv[2], max(d), lit,
          "BLANK" if max(d) <= 8 else "HAS CONTENT"))
except Exception as e:
    print("  %s: %s" % (sys.argv[2], e))
EOF
}

sleep 160
python3 "$T" $PORT tap 160 400 >/dev/null 2>&1   # wake the panel
sleep 5
echo "before reset: banners=$(strings "$D/$TAG.serial" | grep -c 'Darwin Kernel Version')"
shot before

python3 - $PORT <<'EOF'
import socket, json, sys
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=15)
f = s.makefile("rw"); f.readline()
def cmd(e, **a):
    m = {"execute": e}
    if a: m["arguments"] = a
    f.write(json.dumps(m) + "\n"); f.flush()
    while True:
        r = json.loads(f.readline())
        if "event" not in r: return r
cmd("qmp_capabilities"); print("  system_reset ->", cmd("system_reset"))
EOF

sleep 460
python3 "$T" $PORT tap 160 400 >/dev/null 2>&1   # wake the panel again
sleep 8
python3 - "$D/$TAG.serial" <<'PYEOF'
import sys, subprocess
t = subprocess.run(['strings', sys.argv[1]], capture_output=True, text=True).stdout
L = t.split('\n')
st = [i for i, l in enumerate(L) if 'Darwin Kernel Version' in l and 'Kernel version:' not in l]
b = st + [len(L)]
print("  kernel boots: %d" % len(st))
for n in range(len(st)):
    seg = '\n'.join(L[b[n]:b[n+1]])
    print("    boot%d: configd=%d SCHelper=%d CLCD=%d" %
          (n+1, seg.count('configd'), seg.count('SCHelper'), seg.count('unexpected CLCD')))
PYEOF
shot after

kill $PID 2>/dev/null; wait $PID 2>/dev/null
for n in before after; do
  sips -s format png "$D/$TAG-$n.ppm" --out "$D/$TAG-$n.png" >/dev/null 2>&1
done
echo "screens: $D/$TAG-before.png  $D/$TAG-after.png"
