#!/bin/bash
# hw/arm/guest-package.c's core (GUEST_PKG_CORE_ONLY: glib only) on the host under ASan/UBSan against
# guest-package-test.c's guest-memory fixture. Exit 0 = pass.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT
mkdir -p "$T/qemu" "$T/offer"
cat > "$T/qemu/osdep.h" <<'H'
#pragma once
#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <glib.h>
H
clang -g -Wall -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -DGUEST_PKG_CORE_ONLY \
    -I"$T" -I"$ROOT/include" "$ROOT/hw/arm/guest-package.c" "$HERE/guest-package-test.c" -o "$T/check" \
    $(pkg-config --cflags --libs glib-2.0)
"$T/check" "$T/offer"
