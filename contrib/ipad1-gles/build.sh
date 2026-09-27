#!/bin/bash
# Build the iPad (iOS 3.2.2) GLEngine.bundle/GLEngine replacement, armv7, and
# run the offline checks. See README.md and
# docs/ipad1/hw2-regs/README-native-code-on-3.2.2.md for the flags.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
SDK="${IPAD_SDK:-$HOME/Developer/qemu-ios-files/ipad1/sdk/x-iPhoneSDK3_2_2/Payload/Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS3.2.sdk}"
cd "$HERE"

python3 ../it-gles/genstubs.py ../it-gles/gles_stubs.h >/dev/null
python3 gligen.py gli_fwd.h >/dev/null

# A bundle has no LC_MAIN, which 3.2's dyld cannot parse. -marm: 3.2 dyld does
# not interwork into Thumb constructors, and OpenGLES tail-calls our slots.
xcrun clang -arch armv7 -marm -miphoneos-version-min=7.0 -isysroot "$SDK" \
    -O1 -Wall -fno-stack-protector -fno-builtin -bundle -o GLEngine glishim.c
command -v ldid >/dev/null && ldid -S GLEngine

# Offline checks: exactly the 19 gli* names OpenGLES dlsyms, and the table.
strings -a "$SDK/System/Library/Frameworks/OpenGLES.framework/OpenGLES" |
    grep '^gli' | sort -u >want.txt
nm -gU GLEngine | awk '{print substr($3,2)}' | grep '^gli' | sort >have.txt
diff want.txt have.txt && echo "exports OK: $(wc -l <have.txt | tr -d ' ') gli* symbols"
rm -f want.txt have.txt
python3 gligen.py --check
cc -w test_glishim.c -o test_glishim && ./test_glishim 2>/dev/null
rm -f test_glishim
file GLEngine
