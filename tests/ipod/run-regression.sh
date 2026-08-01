#!/bin/bash
#
# Regression harness for the emulated iPod touch 2G - single entry point.
#
#     tests/ipod/run-regression.sh              # everything (~25-40 min)
#     tests/ipod/run-regression.sh --quick      # boot + AFC only, one boot
#     tests/ipod/run-regression.sh --checks boot,wifi
#
# Every check corresponds to a bug that shipped in this tree; see the docstring
# in regress.py for what each one is guarding. Exits non-zero if any selected
# check fails.
#
# Prerequisites:
#   - build/qemu-system-arm built from this tree (configure WITHOUT
#     --disable-slirp; the WiFi check needs -netdev user)
#   - the usbmuxd fork with the QEMU backend, for every USB-side check
#   - libimobiledevice tools on PATH (afcclient, iproxy, ideviceinstaller)
#   - $F/nand-canonical as the base image
#
# The harness picks its own free ports, so concurrent runs do not collide, and
# it only ever signals processes it started itself.
set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PY="${PYTHON:-python3}"

if [ ! -x "$SCRIPT_DIR/../../build/qemu-system-arm" ]; then
    echo "build/qemu-system-arm is missing - build the tree first" >&2
    exit 2
fi

exec "$PY" "$SCRIPT_DIR/regress.py" "$@"
