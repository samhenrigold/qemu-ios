#!/bin/bash
# Build it_kbd_agent.dylib for iPhone OS 2.x (armv6). Needs the 2.0 device SDK
# for framework headers/stubs. Adjust SDK if yours lives elsewhere.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
SDK="${IPHONEOS2_SDK:-$HOME/Downloads/OldSDK/iPhoneOS2.0.sdk}"
OUT="${1:-$HERE/it_kbd_agent.dylib}"

if [ ! -d "$SDK" ]; then
  echo "iPhoneOS 2.0 SDK not found at $SDK (set IPHONEOS2_SDK)"; exit 1
fi

clang -arch armv6 -isysroot "$SDK" \
  -dynamiclib -mios-version-min=2.0 \
  -framework CoreFoundation \
  -o "$OUT" "$HERE/it_kbd_agent.c"

echo "built $OUT"
# Ad-hoc sign so the loader accepts it on the (jailbroken-style) emulator image.
codesign -f -s - "$OUT" 2>/dev/null || echo "(codesign skipped; inject as-is)"
