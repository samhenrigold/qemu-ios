#!/bin/bash
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/../armv6-toolchain/armv6.sh"
cc6 "$HERE/itmedia.c" "$HERE/itmedia.o" -Wall -Wextra \
    -isystem "$(xcrun clang -print-resource-dir)/include"
link6 -execute "$HERE/itmedia" "$HERE/itmedia.o" -e __start
rm -f "$HERE/itmedia.o"
# 3.2+ AMFI runs only signed executables; 3.1 ignores the signature. 5.x's importer signs each track's
# integrity through FairPlay, which answers only fairplay-client processes: the entitlement carries 9B206 atc's
# client ID (the sync agent that imports tracks from iTunes).
"${LDID:-ldid}" -S"$HERE/itmedia.entitlements" "$HERE/itmedia"
cc6 "$HERE/itphoto.c" "$HERE/itphoto.o" -Wall -Wextra
link6 -execute "$HERE/itphoto" "$HERE/itphoto.o" -e __start
rm -f "$HERE/itphoto.o"
"${LDID:-ldid}" -S "$HERE/itphoto"
