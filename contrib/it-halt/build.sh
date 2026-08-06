#!/bin/bash
# Build the guest-side halt helper. See ../armv6-toolchain/README.md.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/../armv6-toolchain/armv6.sh"

# -e __start: there is no crt1, so the binary supplies its own entry point.
cc6 "$HERE/ithalt.c" "$HERE/ithalt.o"
link6 -execute "$HERE/ithalt" "$HERE/ithalt.o" -e __start
rm -f "$HERE/ithalt.o"
file "$HERE/ithalt"
