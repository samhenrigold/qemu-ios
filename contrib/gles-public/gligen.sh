#!/bin/sh
# Check include/hw/arm/guest-services/gles-names.h (gligen.c): gligen.sh [--stamp]
# After a hand edit of the table, --stamp prints the GLES_NAMES_VERSION to write into it.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
TMP="$(mktemp -d "${TMPDIR:-/tmp}/gligen.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT
xcrun clang -std=gnu11 -O1 -Wall -Werror "$HERE/gligen.c" -lz -o "$TMP/gligen"
"$TMP/gligen" "$HERE/../.." "$@"
