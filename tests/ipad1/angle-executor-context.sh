#!/bin/bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
ANGLE="${1:?usage: angle-executor-context.sh ANGLE-SOURCE}"
BUILD="$ANGLE/out/ltm-metal"
TMP="$(mktemp -d /tmp/angle-executor-context.XXXXXX)"
trap 'rm -rf "$TMP"' EXIT
clang -std=gnu11 -I"$ANGLE/include" $(pkg-config --cflags glib-2.0) \
    "$ROOT/tests/ipad1/angle-executor-context.c" -L"$BUILD" -Wl,-rpath,"$BUILD" \
    -lEGL -lGLESv2 $(pkg-config --libs glib-2.0) -o "$TMP/context"
"$TMP/context"
