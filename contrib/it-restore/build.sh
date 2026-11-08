#!/bin/bash
# Source-built restore tools, using the same legacy armv7 pipeline as the other
# guest tools. The shipped tools require neither an SDK nor ldid at runtime.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
: "${ARMV6_SDK:?Set ARMV6_SDK to an SDK with 32-bit iOS headers and libSystem}"
export GUEST_ARCH=armv7 LEGACY_LINK=0
. "$HERE/../armv6-toolchain/armv6.sh"
OUT="${1:?usage: build.sh OUTPUT_DIRECTORY}"
mkdir -p "$OUT"
OBJ="$(mktemp -d /tmp/it-restore.XXXXXX)"
trap 'rm -rf "$OBJ"' EXIT
cc6 "$HERE/../armv6-toolchain/crt1old.c" "$OBJ/start.o" -D_FORTIFY_SOURCE=0
cc6 "$HERE/restore.c" "$OBJ/restore.o" -D_FORTIFY_SOURCE=0 -isystem "$(xcrun clang -print-resource-dir)/include"
cc6 "$HERE/restore.c" "$OBJ/initial.o" -DIT_INITIAL_PREPARE=1 -D_FORTIFY_SOURCE=0 -isystem "$(xcrun clang -print-resource-dir)/include"
cc6 "$HERE/data.c" "$OBJ/data.o" '-DA6_PREPARED_DATA_SEED="/usr/local/share/qemu/data-seed"' -D_FORTIFY_SOURCE=0 -isystem "$(xcrun clang -print-resource-dir)/include"
cc6 "$HERE/mobilegestalt-parent.c" "$OBJ/mobilegestalt.o" -D_FORTIFY_SOURCE=0 -isystem "$(xcrun clang -print-resource-dir)/include"
cc6 "$HERE/mobilegestalt-worker.c" "$OBJ/mobilegestalt-worker.o" -D_FORTIFY_SOURCE=0 -isystem "$(xcrun clang -print-resource-dir)/include"
link6 -execute "$OUT/it_mobilegestalt" "$OBJ/mobilegestalt-worker.o" "$OBJ/start.o" -e _start
"${LDID:-ldid}" "-S$HERE/mobilegestalt-entitlements.plist" "$OUT/it_mobilegestalt"
link6 -execute "$OUT/it_restore" "$OBJ/restore.o" "$OBJ/start.o" -e _start
link6 -execute "$OUT/it_initialize" "$OBJ/initial.o" "$OBJ/data.o" "$OBJ/mobilegestalt.o" "$OBJ/start.o" -e _start
for tool in it_restore it_initialize; do
    if [ -n "${RESTORE_ENTITLEMENTS:-}" ]; then
        "${LDID:-ldid}" "-S$RESTORE_ENTITLEMENTS" "$OUT/$tool"
    else
        "${LDID:-ldid}" "-S$HERE/entitlements.plist" "$OUT/$tool"
    fi
done
