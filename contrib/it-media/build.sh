#!/bin/bash
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/../armv6-toolchain/armv6.sh"
cc6 "$HERE/itmedia.c" "$HERE/itmedia.o" -Wall -Wextra \
    -isystem "$(xcrun clang -print-resource-dir)/include"
link6 -execute "$HERE/itmedia" "$HERE/itmedia.o" -e __start
rm -f "$HERE/itmedia.o"
cc6 "$HERE/itphoto.c" "$HERE/itphoto.o" -Wall -Wextra
link6 -execute "$HERE/itphoto" "$HERE/itphoto.o" -e __start
rm -f "$HERE/itphoto.o"
