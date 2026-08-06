#!/bin/bash
# Build the guest-side orientation reporter. See ../armv6-toolchain/README.md.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/../armv6-toolchain/armv6.sh"

# -e __start: there is no crt1, so the binary supplies its own entry point.
# itorient.c explains why.
cc6 "$HERE/itorient.c" "$HERE/itorient.o"
link6 -execute "$HERE/itorient" "$HERE/itorient.o" -e __start
rm -f "$HERE/itorient.o"
file "$HERE/itorient"
