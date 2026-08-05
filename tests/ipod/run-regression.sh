#!/bin/bash
#
# Regression harness for the emulated iPod touch 2G - single entry point.
#
#     tests/ipod/run-regression.sh               # default tier: boot, fsck, persist
#     tests/ipod/run-regression.sh --with-apps   # + afc, usbtcp, wifi, appinstall, applaunch
#     tests/ipod/run-regression.sh --quick       # boot + AFC only, one boot
#     tests/ipod/run-regression.sh --checks boot,wifi
#     tests/ipod/run-regression.sh --check-prereqs   # what's missing, runs nothing
#
# Every check corresponds to a bug that shipped in this tree; see the docstring
# in regress.py for what each one is guarding. Exits non-zero if any selected
# check FAILs (checks with missing inputs SKIP instead and don't count).
#
# Prerequisites for the default tier:
#   - build/qemu-system-arm built from this tree
#   - $F/nand-canonical as the base image
#
# Prerequisites for --with-apps, on top of the above:
#   - configure WITHOUT --disable-slirp (the wifi check needs -netdev user)
#   - the usbmuxd fork with the QEMU backend, for every USB-side check
#   - libimobiledevice tools on PATH (afcclient, iproxy, ideviceinstaller)
#   - an .ipa via --ipa, for appinstall/applaunch
#
# The harness picks its own free ports, so concurrent runs do not collide, and
# it only ever signals processes it started itself.
set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PY="${PYTHON:-python3}"

check_prereqs=0
for arg in "$@"; do
    [ "$arg" = "--check-prereqs" ] && check_prereqs=1
done

# --check-prereqs is meant to run on a checkout that hasn't built anything
# yet, so it must not be blocked by the same build it's there to report on.
if [ "$check_prereqs" -eq 0 ] && [ ! -x "$SCRIPT_DIR/../../build/qemu-system-arm" ]; then
    echo "build/qemu-system-arm is missing - build the tree first" >&2
    exit 2
fi

exec "$PY" "$SCRIPT_DIR/regress.py" "$@"
