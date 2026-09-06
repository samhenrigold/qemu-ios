#!/bin/bash
#
# Regression harness for the emulated iPod touch 2G - single entry point.
#
#     tests/ipod/run-regression.sh               # default tier: boot, fsck, persist, apps, GLES, agent, audio
#     tests/ipod/run-regression.sh --with-apps   # + afc, usbtcp, wifi, respring, restart
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
#   - a matching base NAND with it_agent (prefers nand-agent-v2)
#
# Additional check prerequisites (missing host inputs SKIP):
#   - configure WITHOUT --disable-slirp (the wifi check needs -netdev user)
#   - the usbmuxd fork with the QEMU backend, for every USB-side check
#   - libimobiledevice tools on PATH (afcclient, iproxy, ideviceinstaller)
#   - contrib/it-harness/build/Harness.ipa (or --ipa for app checks)
#   - NumPy for stereo audio spectrum validation
#
# The harness picks its own free ports, so concurrent runs do not collide, and
# it only ever signals processes it started itself.
set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PY="${PYTHON:-python3}"

# regress.py validates the selected --qemu path, including explicit overrides.
exec "$PY" "$SCRIPT_DIR/regress.py" "$@"
