#!/bin/bash
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/../armv6-toolchain/armv6.sh"
cc6 "$HERE/it_agent.c" "$HERE/it_agent.o" -isystem "$(xcrun clang -print-resource-dir)/include"
link6 -execute "$HERE/it_agent" "$HERE/it_agent.o"
rm -f "$HERE/it_agent.o"

"${LDID:-ldid}" -S"$HERE/it_agent-entitlements.xml" "$HERE/it_agent"
cc6 "$HERE/it_typein.c" "$HERE/it_typein.o"
link6 -dylib "$HERE/it_typein.dylib" "$HERE/it_typein.o" -install_name /usr/lib/it_typein.dylib
rm -f "$HERE/it_typein.o"

# 2.x enforces executable-page signatures even where later kernels accept
# cs_enforcement_disable. This injected dylib must be signed too.
"${LDID:-ldid}" -S "$HERE/it_typein.dylib"
