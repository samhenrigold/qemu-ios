#!/bin/bash
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/../armv6-toolchain/armv6.sh"
cc6 "$HERE/itmedia.c" "$HERE/itmedia.o" -Wall -Wextra \
    -isystem "$(xcrun clang -print-resource-dir)/include"
link6 -execute "$HERE/itmedia" "$HERE/itmedia.o" -e __start
rm -f "$HERE/itmedia.o"
