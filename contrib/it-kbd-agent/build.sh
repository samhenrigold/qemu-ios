#!/bin/bash
# Build it_kbd_agent.dylib for the iPod Touch 2G (armv6, ARM1176 -> armv6 ONLY).
#
# The catch: a modern macOS `ld` refuses to link armv6 ("linking for armv6 is no
# longer supported"). You need an old linker. Two paths:
#
# 1. REMOTE (used to produce the checked-in dylib): a Snow Leopard 10.6.8 box with
#    Xcode 4.2 (arm-apple-darwin10 clang + ld64-127.2, which still links armv6).
#    The iPhoneOS5.0 SDK links armv6 fine with -miphoneos-version-min=2.0, and the
#    agent only touches stable CoreFoundation + dlsym, so it runs on 2.1.1.
#    Set REMOTE=<ssh-host> to cross-build there:
#        REMOTE=tiger ./build.sh
#
# 2. LOCAL: only works if your host toolchain has an armv6-capable linker
#    (cctools-port built with armv6, or an old Xcode). Uses IPHONEOS2_SDK.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${1:-$HERE/it_kbd_agent.dylib}"

if [ -n "${REMOTE:-}" ]; then
  P=/Developer/Platforms/iPhoneOS.platform/Developer
  SDK="${REMOTE_SDK:-$P/SDKs/iPhoneOS5.0.sdk}"
  cat "$HERE/it_kbd_agent.c" | ssh "$REMOTE" 'cat > /tmp/it_kbd_agent.c'
  ssh "$REMOTE" "$P/usr/bin/clang -arch armv6 -isysroot $SDK \
      -miphoneos-version-min=2.0 -dynamiclib -framework CoreFoundation -lobjc \
      -o /tmp/it_kbd_agent.dylib /tmp/it_kbd_agent.c && \
      codesign -f -s - /tmp/it_kbd_agent.dylib"
  scp "$REMOTE:/tmp/it_kbd_agent.dylib" "$OUT"
  echo "built (remote $REMOTE) $OUT"
else
  SDK="${IPHONEOS2_SDK:-$HOME/Downloads/OldSDK/iPhoneOS2.0.sdk}"
  clang -arch armv6 -isysroot "$SDK" -dynamiclib -mios-version-min=2.0 \
    -framework CoreFoundation -o "$OUT" "$HERE/it_kbd_agent.c"
  codesign -f -s - "$OUT" 2>/dev/null || echo "(codesign skipped)"
  echo "built (local) $OUT"
fi
