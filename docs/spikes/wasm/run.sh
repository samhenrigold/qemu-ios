#!/bin/bash
# Boots n72ap-7E18 headlessly as LightTouchMac's helper does (BootRecipe.iPod), silent,
# with a fresh overlay and NOR clone in $RUN, and the spike probe on stderr.
# usage: run.sh NAME SECONDS -- QEMU-COMMAND... [-- EXTRA QEMU ARGS]
#   QEMU-COMMAND: e.g. build-native/qemu-system-arm, or "node --stack-size=... build-wasm/qemu-system-arm.js"
# env: KEEP=1 reuses $RUN's overlay/NOR (for resume), BASE overrides the gate base.
set -euo pipefail
NAME=$1 SECS=$2; shift 3
CMD=(); while [ $# -gt 0 ] && [ "$1" != "--" ]; do CMD+=("$1"); shift; done; [ $# -gt 0 ] && shift
BASE=${BASE:-$HOME/Developer/qemu-ios-files/m68/ltm-1x/n72-7E18}
ROM=$HOME/Developer/qemu-ios-files/bootrom_240_4
RUN=$HOME/Developer/wasm-spike-run/$NAME
if [ -z "${KEEP:-}" ]; then
  rm -rf "$RUN"; mkdir -p "$RUN/overlay"
  cp -c "$BASE/nor.bin" "$RUN/nor-rw.bin"; chmod u+w "$RUN/nor-rw.bin"
fi
M="iPod-Touch,boot-args=amfi_allow_any_signature=1 cs_enforcement_disable=1,direct-iboot=$BASE/iBoot.bin,direct-llb="
M+=",bootrom=$ROM,nand=$BASE/nand,nor=$BASE/nor.bin,nor-rw=$RUN/nor-rw.bin,nandrw=$RUN/overlay,gid-blobs=$BASE/gid-blobs.bin"
START=$(python3 -c 'import time;print(time.time())')
IT_TVOUT_READY=1 SPIKE_PROBE=$SECS SPIKE_SHOT=$RUN/screen.ppm "${CMD[@]}" -M "$M" -m 128M -display none -no-shutdown \
  -audio none -serial "file:$RUN/serial.log" "$@" 2> "$RUN/stderr.log" || true
END=$(python3 -c 'import time;print(time.time())')
# First lit second (luma > 0.03 after a dark spell) and the process's wall time.
awk -v s="$START" -v e="$END" -v n="$NAME" '
  /^spike / { split($2,a,"="); split($5,l,"="); t=a[2]; if (l[2] <= 0.03) dark=1; else if (dark && !lit) lit=t; last=$0 }
  END { printf "%s: wall %.1f s, first lit after dark at probe t=%s, last: %s\n", n, e-s, lit ? lit : "never", last }' "$RUN/stderr.log"
