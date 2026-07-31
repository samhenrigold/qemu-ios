#!/bin/bash
# Boot nand-apps2, wake/unlock, and launch AwesomeBall, capturing serial + MBX trace.
#   run-app.sh <tag> [extra machine opts, e.g. ,mbx-irq=on]
# The device auto-locks and blanks the screen, so it has to be woken and
# unlocked before any tap reaches SpringBoard. Screenshots are taken at each
# step so the sequence can be checked afterwards.
set -u
TAG="$1"
EXTRA="${2:-}"
D="/private/tmp/claude-501/-Users-shg-Developer-qemu-ios/ebb367f9-df80-473b-a8c5-d32a64cc0728/scratchpad"
FILES="$HOME/Developer/qemu-ios-files"
QEMU="$HOME/Developer/wt-nand/build/arm-softmmu/qemu-system-arm"
T="$HOME/Developer/wt-nand/qmp-touch.py"
PORT=4520

shot() { python3 "$T" $PORT shot "$D/$TAG-$1.ppm" >/dev/null 2>&1 || true; echo "  shot $1"; }

"$QEMU" \
  -M iPod-Touch,bootrom="$FILES/bootrom_240_4",nand="$FILES/nand-apps2",nor="$FILES/nor_n72ap.bin"$EXTRA \
  -serial file:"$D/$TAG.serial" -cpu max -m 2G -display none \
  -qmp tcp:127.0.0.1:$PORT,server,nowait \
  > "$D/$TAG.stdout" 2> "$D/$TAG.mbxtrace" &
PID=$!
echo "qemu pid=$PID tag=$TAG"

sleep 120
shot 00-boot

# The device auto-locks and blanks the screen; a tap wakes it straight onto
# the home screen. AwesomeBall sits on page 1, third row, second column, behind
# the first-run "Edit Home Screen" dialog, so dismiss that first.
python3 "$T" $PORT tap 160 400 >/dev/null 2>&1
sleep 3
shot 01-wake
python3 "$T" $PORT tap 160 327 >/dev/null 2>&1   # Dismiss
sleep 3
shot 02-dismissed

echo "MBX accesses before launch: $(grep -c '\[MBX\]' "$D/$TAG.mbxtrace")"
python3 "$T" $PORT tap 122 330 >/dev/null 2>&1
echo "tapped AwesomeBall"
sleep 30
shot 05-app
sleep 30
shot 06-app

echo "MBX accesses after launch: $(grep -c '\[MBX\]' "$D/$TAG.mbxtrace")"
kill $PID 2>/dev/null
wait $PID 2>/dev/null
echo "stopped $PID"
