#!/bin/bash
# A failed guest-helper compile must not silently reuse an old object; extra compiler flags reach clang.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT
cc() { bash -c 'source "$1"; cc6 "$2" "$3" -DCHECK_VALUE=7' check "$HERE/../../contrib/armv6-toolchain/armv6.sh" "$T/input.c" "$T/output.o"; }
echo 'this is deliberately invalid C;' > "$T/input.c"
printf 'stale object' > "$T/output.o"
if cc 2>/dev/null; then echo "FAIL: invalid C compiled" >&2; exit 1; fi
[ ! -e "$T/output.o" ] || { echo "FAIL: stale object left behind" >&2; exit 1; }
echo 'int check(void) { return CHECK_VALUE; }' > "$T/input.c"
cc || { echo "FAIL: valid C with -DCHECK_VALUE did not compile" >&2; exit 1; }
[ -s "$T/output.o" ] || { echo "FAIL: no object" >&2; exit 1; }
! ls "$T"/*.cclog >/dev/null 2>&1 || { echo "FAIL: compiler log left behind" >&2; exit 1; }
echo 'PASS: failed ARMv6 compiles reject stale objects; extra compiler flags are forwarded'
