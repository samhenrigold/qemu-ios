#!/bin/bash
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/../armv6-toolchain/armv6.sh"
cc6 "$HERE/itproxy.c" "$HERE/itproxy.o"
link6 -execute "$HERE/itproxy" "$HERE/itproxy.o" -e __start
rm -f "$HERE/itproxy.o"
cc6 "$HERE/httpget.c" "$HERE/httpget.o"
link6 -execute "$HERE/httpget" "$HERE/httpget.o" -e __start
rm -f "$HERE/httpget.o"
