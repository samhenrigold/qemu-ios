#!/bin/bash
# Build the guest-side halt helper. See ../armv6-toolchain/README.md.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/../armv6-toolchain/armv6.sh"

# -e __start: there is no crt1, so the binary supplies its own entry point.
for tool in ithalt itbattery; do
    cc6 "$HERE/$tool.c" "$HERE/$tool.o"
    link6 -execute "$HERE/$tool" "$HERE/$tool.o" -e __start
    rm -f "$HERE/$tool.o"
    file "$HERE/$tool"
done
