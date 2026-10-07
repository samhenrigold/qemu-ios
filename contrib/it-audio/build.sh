#!/bin/bash
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
source "$here/../armv6-toolchain/armv6.sh"
out="${1:-/tmp/it-audio-offline}"
resource="$(xcrun clang -print-resource-dir)"
cc6 "$here/offline.c" "$out.o" \
    -isystem "$resource/include" -F "$ARMV6_SDK/System/Library/Frameworks"
link6 -execute "$out" "$out.o"
rm -f "$out.o"
