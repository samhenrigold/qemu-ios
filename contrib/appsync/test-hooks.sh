#!/bin/bash
# Exercise ownership and original-first behavior with real macOS CF objects.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT=$(mktemp -d "${TMPDIR:-/tmp}/appsync-hooks.XXXXXX")
trap 'rm -rf "$OUT"' EXIT
xcrun clang -g -fsanitize=address,undefined "$HERE/test-hooks.c" \
    -framework CoreFoundation -o "$OUT/test-hooks"
"$OUT/test-hooks"
