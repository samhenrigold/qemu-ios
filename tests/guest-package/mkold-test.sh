#!/bin/bash
# Build machotool and mkold-test.c under ASan/UBSan and run the test (exit 0 = pass).
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT
SAN=(-std=c11 -g -O1 -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all)
xcrun clang "${SAN[@]}" -o "$T/machotool" "$HERE/../../contrib/armv6-toolchain/machotool.c"
xcrun clang "${SAN[@]}" -o "$T/mkold-test" "$HERE/mkold-test.c"
"$T/mkold-test" "$T/machotool"
