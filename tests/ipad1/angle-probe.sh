#!/bin/bash
# Rendering probe, separate from guest compatibility acceptance.
set -euo pipefail
ANGLE="${1:?usage: angle-probe.sh ANGLE-CHECKOUT [BUILD-DIRECTORY]}"
BUILD="${2:-$ANGLE/out/ltm-metal}"
HERE="$(cd "$(dirname "$0")" && pwd)"
TMP="$(mktemp -d "${TMPDIR:-/tmp}/angle-probe.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT
clang -DGL_GLES_PROTOTYPES=1 "$HERE/angle-probe.c" -I "$ANGLE/include" \
    -L "$BUILD" -lEGL -lGLESv2 -Wl,-rpath,"$BUILD" -o "$TMP/probe"
"$TMP/probe"

# Context/sharegroup and surface contracts still needed by the guest transport.
clang -DGL_GLES_PROTOTYPES=1 "$HERE/angle-context-contract.c" -I "$ANGLE/include" \
    -L "$BUILD" -lEGL -lGLESv2 -Wl,-rpath,"$BUILD" -o "$TMP/context-contract"
"$TMP/context-contract"
