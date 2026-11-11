#!/bin/bash
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
export GUEST_ARCH=armv7
. "$HERE/../armv6-toolchain/armv6.sh"
OUT="${1:-$HERE/launchd-cache-template}"
PAYLOAD="$(mktemp)"
trap 'rm -f "$PAYLOAD" "$OUT.o"' EXIT
python3 -c 'import sys; open(sys.argv[1],"wb").write(bytes(1<<20))' "$PAYLOAD"
cc6 "$HERE/cache-marker.c" "$OUT.o"
link6 -dylib "$OUT" "$OUT.o" -install_name /System/Library/Caches/com.apple.xpcd/xpcd_cache.dylib -sectcreate __TEXT __xpcd_cache "$PAYLOAD"
"${LDID:-ldid}" -S "$OUT"
