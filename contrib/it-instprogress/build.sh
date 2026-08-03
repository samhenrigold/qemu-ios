#!/bin/bash
# Build the guest-side install-progress binaries. See ../armv6-toolchain/README.md.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/../armv6-toolchain/armv6.sh"

for t in sbdlicon sbunlock; do
    cc6 "$HERE/$t.c" "$HERE/$t.o"
    link6 -execute "$HERE/$t" "$HERE/$t.o" -e __start
    rm -f "$HERE/$t.o"
    file "$HERE/$t"
done
