#!/bin/bash
# Build the guest-side GLES test binaries. See ../armv6-toolchain/README.md.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/../armv6-toolchain/armv6.sh"

for t in gles_tri gles_tex; do
    cc6 "$HERE/$t.c" "$HERE/$t.o"
    link6 -execute "$HERE/$t" "$HERE/$t.o"
    rm -f "$HERE/$t.o"
done
file "$HERE/gles_tri" "$HERE/gles_tex"
