#!/bin/bash
# Boot the emulator for NAND-persistence testing.
#   run-nand.sh <logfile> <seconds> [extra qemu args...]
# Takes a QMP screendump to <logfile>.ppm just before shutting down.
# Uses ONLY the ovl-nand overlay and QMP port 4500. Kills only the PID it starts.
set -u
LOG="$1"; shift
SECS="$1"; shift

FILES="$HOME/Developer/qemu-ios-files"
QEMU="$HOME/Developer/wt-nand/build/arm-softmmu/qemu-system-arm"
PORT=4500

"$QEMU" \
  -M iPod-Touch,bootrom="$FILES/bootrom_240_4",nand="$FILES/nand",nor="$FILES/nor_n72ap.bin",nandrw="$FILES/ovl-nand" \
  -serial file:"$LOG" -cpu max -m 2G -display none \
  -qmp tcp:127.0.0.1:$PORT,server,nowait \
  "$@" > "${LOG}.stdout" 2>&1 &
PID=$!
echo "qemu pid=$PID log=$LOG"
sleep "$SECS"

python3 - "$PORT" "${LOG}.ppm" <<'PYEOF' || echo "screendump failed"
import socket, json, sys, time
port = int(sys.argv[1]); out = sys.argv[2]
s = socket.create_connection(('127.0.0.1', port), timeout=10)
f = s.makefile('rw')
f.readline()
f.write(json.dumps({"execute": "qmp_capabilities"}) + "\n"); f.flush(); f.readline()
f.write(json.dumps({"execute": "screendump", "arguments": {"filename": out}}) + "\n"); f.flush()
print("screendump:", f.readline().strip())
s.close()
PYEOF

kill "$PID" 2>/dev/null
wait "$PID" 2>/dev/null
echo "stopped $PID"
