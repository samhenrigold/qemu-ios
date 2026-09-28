#!/bin/bash
# Build the iPad GLEngine.bundle/GLEngine replacements (one per firmware dispatch layout), armv7, and
# run the offline checks. See README.md and
# docs/ipad1/hw2-regs/README-native-code-on-3.2.2.md for the flags.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
SDK="${IPAD_SDK:-$HOME/Developer/qemu-ios-files/ipad1/sdk/x-iPhoneSDK3_2_2/Payload/Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS3.2.sdk}"
cd "$HERE"

python3 ../it-gles/genstubs.py ../it-gles/gles_stubs.h >/dev/null

# One engine per dispatch layout: GLEngine-<BUILD> from docs/ipad1/gli-dispatch-<BUILD>.tsv
# (7B500's serves 7B367 too; ipad1_rootfs.py picks the one whose fields match the firmware).
for tsv in ../../docs/ipad1/gli-dispatch-*.tsv; do
    b="${tsv##*gli-dispatch-}"; b="${b%.tsv}"
    python3 gligen.py --tsv "$tsv" gli_fwd.h >/dev/null
    # A bundle has no LC_MAIN, which 3.2's dyld cannot parse. -marm: 3.2 dyld does
    # not interwork into Thumb constructors, and OpenGLES tail-calls our slots.
    xcrun clang -arch armv7 -marm -miphoneos-version-min=7.0 -isysroot "$SDK" \
        -O1 -Wall -fno-stack-protector -fno-builtin -bundle -o "GLEngine-$b" glishim.c
    command -v ldid >/dev/null && ldid -S "GLEngine-$b"

    # Offline checks: the gli* names OpenGLES dlsyms (3.2's 19, plus 4.x's
    # gliCreateContextWithShared), and the table.
    { strings -a "$SDK/System/Library/Frameworks/OpenGLES.framework/OpenGLES" | grep '^gli'
      echo gliCreateContextWithShared; } | sort -u >want.txt
    nm -gU "GLEngine-$b" | awk '{print substr($3,2)}' | grep '^gli' | sort >have.txt
    diff want.txt have.txt && echo "GLEngine-$b exports OK: $(wc -l <have.txt | tr -d ' ') gli* symbols"
    rm -f want.txt have.txt
    python3 gligen.py --tsv "$tsv" --check
    cc -w test_glishim.c -o test_glishim && ./test_glishim 2>/dev/null
    rm -f test_glishim
done

# The gld plugin 4.x's libGFXShared needs (gldshim.c), as the bundle rootfs builds install.
rm -rf GLRendererFloatQEMU.bundle && mkdir GLRendererFloatQEMU.bundle
xcrun clang -arch armv7 -marm -miphoneos-version-min=7.0 -isysroot "$SDK" \
    -O1 -Wall -fno-stack-protector -fno-builtin -bundle \
    -o GLRendererFloatQEMU.bundle/GLRendererFloatQEMU gldshim.c
command -v ldid >/dev/null && ldid -S GLRendererFloatQEMU.bundle/GLRendererFloatQEMU
echo "gldshim exports: $(nm -gU GLRendererFloatQEMU.bundle/GLRendererFloatQEMU | grep -c ' _gld') gld* symbols"

# Test apps: contrib/it-gles/glapp.c as GLTest.app (ES1, cyan on magenta) and
# GLTest2.app (-DGLAPP_ES2: blue on yellow). armv6 through contrib/armv6-toolchain,
# whose mkold.py makes an executable 3.2's dyld accepts (no LC_MAIN).
export ARMV6_SDK="$SDK"
. ../armv6-toolchain/armv6.sh
# Log to /dev/console: serial is the only place an app's stderr can be read.
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
file GLEngine-* GLTest.app/GLTest GLTest2.app/GLTest2
