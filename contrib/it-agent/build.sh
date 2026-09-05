#!/bin/bash
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/../armv6-toolchain/armv6.sh"
cc6 "$HERE/it_agent.c" "$HERE/it_agent.o"
link6 -execute "$HERE/it_agent" "$HERE/it_agent.o"
rm -f "$HERE/it_agent.o"

"${LDID:-ldid}" -S"$HERE/../it-gles/sblaunch-entitlements.xml" "$HERE/it_agent"
