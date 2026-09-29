#!/bin/bash
# Build the guest-side install-progress binaries. See ../armv6-toolchain/README.md.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/../armv6-toolchain/armv6.sh"

# -e __start: there is no crt1, so sbdlicon supplies its own entry point.
# sbdlicon.c explains why.
cc6 "$HERE/sbdlicon.c" "$HERE/sbdlicon.o"
link6 -execute "$HERE/sbdlicon" "$HERE/sbdlicon.o" -e __start
rm -f "$HERE/sbdlicon.o"
file "$HERE/sbdlicon"
