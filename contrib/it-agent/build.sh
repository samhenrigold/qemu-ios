#!/bin/bash
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/../armv6-toolchain/armv6.sh"
cc6 "$HERE/it_agent.c" "$HERE/it_agent.o" -isystem "$(xcrun clang -print-resource-dir)/include"
# it_agent.c's _start hands main argc, argv and envp; a legacy link enters at crt1old.c's instead
agent_entry=(); [ "${LEGACY_LINK:-0}" = 1 ] || agent_entry=(-e __start)
link6 -execute "$HERE/it_agent" "$HERE/it_agent.o" ${agent_entry[@]+"${agent_entry[@]}"}
rm -f "$HERE/it_agent.o"

"${LDID:-ldid}" -S"$HERE/it_agent-entitlements.xml" "$HERE/it_agent"
cc6 "$HERE/it_typein.c" "$HERE/it_typein.o"
link6 -dylib "$HERE/it_typein.dylib" "$HERE/it_typein.o" -install_name /usr/lib/it_typein.dylib
rm -f "$HERE/it_typein.o"

# 2.x enforces executable-page signatures even where later kernels accept
# cs_enforcement_disable. This injected dylib must be signed too.
"${LDID:-ldid}" -S "$HERE/it_typein.dylib"
