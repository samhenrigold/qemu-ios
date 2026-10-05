#!/bin/bash
# Prepare from the shared catalog, then require every selected acceptance check.
# Usage: tests/fresh-device.sh BOARD-BUILD IPSW OUT [firmwarekit create options]
# FIRMWAREKIT, FIRMWAREKIT_CATALOG, QEMU, USBMUXD, CHECKS may select built tools.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
if [ "$#" -lt 3 ]; then echo "usage: $0 BOARD-BUILD IPSW OUT [create options]" >&2; exit 2; fi
ENTRY="$1"; IPSW="$2"; OUT="$3"; shift 3
CLI="${FIRMWAREKIT:-firmwarekit}"
CATALOG="${FIRMWAREKIT_CATALOG:-$HOME/Developer/LightTouchMac-multidevice/LightTouchMac/Resources/firmware-catalog.json}"
DEV="$OUT/device"
mkdir -p "$OUT"
if [ ! -f "$DEV/device.lock.json" ]; then
    "$CLI" create --catalog "$CATALOG" --id "$ENTRY" --ipsw "$IPSW" --out "$DEV" "$@" > "$OUT/prepare.jsonl"
fi
python3 - "$DEV" "$ENTRY" <<'PY'
import json,sys
from pathlib import Path
lock=json.loads((Path(sys.argv[1])/'device.lock.json').read_text())
assert lock['board']+'-'+lock['build']==sys.argv[2], 'existing device is for a different catalog entry'
assert lock['inputs'].get('lockdown') is None and lock['inputs'].get('stash') is None, 'not a fresh synthesized device'
print('PASS lock: '+sys.argv[2])
PY
EXTRA=()
case "$ENTRY" in
 k48ap-*) DRIVER="$ROOT/tests/ipad1/regress.py"; DEFAULT_CHECKS=boot,persist ;;
 n81ap-*) DRIVER="$ROOT/tests/ipad1/regress.py"; DEFAULT_CHECKS=boot,persist; EXTRA=(--machine iPod-Touch-4G) ;;
 n90ap-*) DRIVER="$ROOT/tests/ipad1/regress.py"; DEFAULT_CHECKS=boot,persist; EXTRA=(--machine iPhone-4) ;;
 n72ap-*) DRIVER="$ROOT/tests/ipod/regress.py"; DEFAULT_CHECKS=boot,fsck,persist ;;
 n45ap-*) DRIVER="$ROOT/tests/ipod/regress.py"; DEFAULT_CHECKS=boot ;;
 *) echo "unsupported board: $ENTRY" >&2; exit 2 ;;
esac
FLAGS=("${EXTRA[@]}" --device "$DEV" --qemu "${QEMU:-$ROOT/build/qemu-system-arm}" --checks "${CHECKS:-$DEFAULT_CHECKS}" --require-inputs --out "$OUT/regress")
[ -z "${USBMUXD:-}" ] || FLAGS+=(--usbmuxd "$USBMUXD")
python3 "$DRIVER" "${FLAGS[@]}"
