#!/bin/bash
# mkpkg selfcheck on this tree: the itpack round trip, the job rewrite, the Mach-O checks and the family table.
set -eu
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
TMP="$(mktemp -d "${TMPDIR:-/tmp}/mkpkg.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT
xcrun clang -O2 -Wall -Werror -fsanitize=address,undefined -o "$TMP/mkpkg" "$ROOT/contrib/guest-package/mkpkg.c" \
    -lz -framework CoreFoundation
"$TMP/mkpkg" selfcheck "$ROOT"
