#!/bin/bash
# The iPad GL test apps (imgtools/ipad1_rootfs.py build --gles): contrib/it-gles/glapp.c as GLTest.app (ES1, cyan on
# magenta) and GLTest2.app (-DGLAPP_ES2: blue on yellow), armv7 through contrib/armv6-toolchain against the 3.2 SDK
# (mkold.py makes an executable 3.2's dyld accepts: no LC_MAIN). They log to /dev/console, the serial log.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
export ARMV6_SDK="${IPAD_SDK:-$HOME/Developer/qemu-ios-files/ipad1/sdk/x-iPhoneSDK3_2_2/Payload/Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS3.2.sdk}"
. "$HERE/../armv6-toolchain/armv6.sh"
cd "$HERE"
for v in GLTest:: GLTest2:-DGLAPP_ES2:2; do
    IFS=: read -r name def suffix <<<"$v"
    cc6 ../it-gles/glapp.c "$name.o" $def "-DGLAPP_LOG=\"/dev/console\""
    link6 -execute "$name.bin" "$name.o"
    rm -rf "$name.app" "$name.o" && mkdir "$name.app"
    mv "$name.bin" "$name.app/$name"
    sed -e "s/<string>GLTest<\/string>/<string>$name<\/string>/" \
        -e "s/com.qemuios.gltest</com.qemuios.gltest$suffix</" \
        ../it-gles/glapp-Info.plist >"$name.app/Info.plist"
    ldid -S "$name.app/$name"
done
file GLTest.app/GLTest GLTest2.app/GLTest2
