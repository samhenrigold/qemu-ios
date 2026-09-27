#!/bin/bash
# armv7 builds of the iPod guest tools for the iPad 1 on iOS 3.2.2. Same sources,
# same armv6-toolchain pipeline with GUEST_ARCH=armv7 against the 3.2 SDK.
# Output: build/ipad1-guest/ (untracked). Baked by `ipad1_rootfs.py bake`.
# See docs/ipad1/guest-services.md for what is ported and what is not.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
C="$HERE/.."
OUT="${1:-$HERE/../../build/ipad1-guest}"
export GUEST_ARCH=armv7
export ARMV6_SDK="${IPAD_SDK:-$HOME/Developer/qemu-ios-files/ipad1/sdk/x-iPhoneSDK3_2_2/Payload/Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS3.2.sdk}"
. "$C/armv6-toolchain/armv6.sh"
mkdir -p "$OUT"
ENT="$C/it-gles/sblaunch-entitlements.xml"

# name  source  link kind + extra flags
exe() { cc6 "$2" "$OUT/$1.o" "${@:4}"; link6 -execute "$OUT/$1" "$OUT/$1.o" $3; rm -f "$OUT/$1.o"; }
lib() { cc6 "$2" "$OUT/$1.o"; link6 -dylib "$OUT/$1" "$OUT/$1.o" -install_name "$3"; rm -f "$OUT/$1.o"; }

exe it_agent  "$C/it-agent/it_agent.c"         "" -isystem "$(xcrun clang -print-resource-dir)/include"
lib it_typein.dylib "$C/it-agent/it_typein.c"  /usr/lib/it_typein.dylib
exe sblaunch  "$C/it-gles/sblaunch.c"          ""
exe sbdlicon  "$C/it-instprogress/sbdlicon.c"  "-e __start"
exe sbunlock  "$C/it-instprogress/sbunlock.c"  "-e __start"
exe itstatus  "$C/it-status/itstatus.c"        "-e _main"
exe itorient  "$C/it-orientation/itorient.c"   "-e __start"
exe ithalt    "$C/it-halt/ithalt.c"            "-e __start"
exe itbattery "$C/it-halt/itbattery.c"         "-e __start"

LDID="${LDID:-ldid}"
for f in "$OUT"/*; do
    case "${f##*/}" in
        it_agent|sblaunch) "$LDID" "-S$ENT" "$f" ;;
        *) "$LDID" -S "$f" ;;
    esac
done
file "$OUT"/*
