#!/bin/bash
# Build hidbridge.dylib for iOS 3.2.2 (armv7). Flags: docs/ipad1/hw2-regs/README-native-code-on-3.2.2.md
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
SDK="${IPAD_SDK:-$HOME/Developer/qemu-ios-files/ipad1/sdk/x-iPhoneSDK3_2_2/Payload/Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS3.2.sdk}"
cd "$HERE"
xcrun clang -arch armv7 -marm -miphoneos-version-min=7.0 -isysroot "$SDK" -O1 -Wall -fno-stack-protector \
    -dynamiclib -install_name /usr/local/lib/hidbridge.dylib -o hidbridge.dylib hidbridge.c
ldid -S hidbridge.dylib
file hidbridge.dylib
