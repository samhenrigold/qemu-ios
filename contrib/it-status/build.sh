#!/bin/bash
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/../armv6-toolchain/armv6.sh"
cc6 "$HERE/itstatus.c" "$HERE/itstatus.o"
link6 -execute "$HERE/itstatus" "$HERE/itstatus.o" -e _main
rm -f "$HERE/itstatus.o"
