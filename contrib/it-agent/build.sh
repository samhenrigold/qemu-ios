#!/bin/bash
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/../armv6-toolchain/armv6.sh"
cc6 "$HERE/it_agent.c" "$HERE/it_agent.o"
link6 -execute "$HERE/it_agent" "$HERE/it_agent.o"
rm -f "$HERE/it_agent.o"

"${LDID:-ldid}" -S"$HERE/../it-gles/sblaunch-entitlements.xml" "$HERE/it_agent"
cc6 "$HERE/it_typein.c" "$HERE/it_typein.o"
link6 -dylib "$HERE/it_typein.dylib" "$HERE/it_typein.o" -install_name /usr/lib/it_typein.dylib
rm -f "$HERE/it_typein.o"
