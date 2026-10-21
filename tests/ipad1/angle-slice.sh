#!/bin/bash
# Slice checks of the GL bridge built for ANGLE (tests/slice/run.sh with $ANGLE in their CFLAGS).
#   angle-slice.sh ANGLE-CHECKOUT [TEST.c...]     default: every tests/ipad1/angle-gles-*.c
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
export ANGLE="$(cd "${1:?usage: angle-slice.sh ANGLE-CHECKOUT [TEST.c...]}" && pwd)"
shift
[ $# -gt 0 ] || set -- "$ROOT"/tests/ipad1/angle-gles-*.c
for t in "$@"; do "$ROOT/tests/slice/run.sh" "$t" && echo "PASS $(basename "$t")"; done
