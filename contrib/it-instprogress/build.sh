#!/bin/bash
# Build the guest-side install-progress binaries. See ../armv6-toolchain/README.md.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/../armv6-toolchain/armv6.sh"

# -e __start: there is no crt1, so each of these supplies its own entry point.
# sbdlicon.c explains why.
for t in sbdlicon sbunlock; do
    cc6 "$HERE/$t.c" "$HERE/$t.o"
    link6 -execute "$HERE/$t" "$HERE/$t.o" -e __start
    rm -f "$HERE/$t.o"
    file "$HERE/$t"
done

# The DYLD_INSERT_LIBRARIES probe. Not part of the placeholder feature -- it
# answers the one question the progress bar depends on. See README.md.
cc6 "$HERE/probe_insert.c" "$HERE/probe_insert.o"
link6 -dylib "$HERE/probe_insert.dylib" "$HERE/probe_insert.o" \
    -install_name /usr/lib/it-probe-insert.dylib
rm -f "$HERE/probe_insert.o"
file "$HERE/probe_insert.dylib"

# The progress-bar dylib, injected into itunesstored the same way. Its
# install_name is where com.apple.itunesstored.plist's DYLD_INSERT_LIBRARIES
# points, so the two have to agree.
cc6 "$HERE/isprogress.c" "$HERE/isprogress.o"
link6 -dylib "$HERE/isprogress.dylib" "$HERE/isprogress.o" \
    -install_name /usr/lib/it-isprogress.dylib
rm -f "$HERE/isprogress.o"
file "$HERE/isprogress.dylib"
