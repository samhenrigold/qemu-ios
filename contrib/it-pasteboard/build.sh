#!/bin/bash
# Build the guest-side pasteboard binaries. See ../armv6-toolchain/README.md.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/../armv6-toolchain/armv6.sh"

for t in pbprobe pbset it_pbd; do
    [ -f "$HERE/$t.c" ] || continue
    cc6 "$HERE/$t.c" "$HERE/$t.o"
    link6 -execute "$HERE/$t" "$HERE/$t.o"
    rm -f "$HERE/$t.o"
    file "$HERE/$t"
done
