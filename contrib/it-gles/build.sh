#!/bin/bash
# Build the guest-side GLES test binaries. See ../armv6-toolchain/README.md.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/../armv6-toolchain/armv6.sh"

for t in gles_tri gles_tex gles_surf; do
    cc6 "$HERE/$t.c" "$HERE/$t.o"
    link6 -execute "$HERE/$t" "$HERE/$t.o"
    rm -f "$HERE/$t.o"
done

# gles_fw needs the ObjC runtime. It does NOT link OpenGLES: `ld -framework
# OpenGLES` against the 3.1.3 SDK is a hard error (2009 dylib reads as platform
# 'unknown', which is fatal for a framework though only a warning for -l), so
# the framework is dlopen'd at runtime instead.
cc6 "$HERE/gles_fw.c" "$HERE/gles_fw.o"
link6 -execute "$HERE/gles_fw" "$HERE/gles_fw.o"
rm -f "$HERE/gles_fw.o"

# The MBXGLEngine.bundle replacement. -bundle, and the install name does not
# matter: the framework dlopens it by path out of the .bundle directory.
python3 "$HERE/genstubs.py" "$HERE/gles_stubs.h" >/dev/null
cc6 "$HERE/mbxshim.c" "$HERE/mbxshim.o"
link6 -bundle "$HERE/MBXGLEngine" "$HERE/mbxshim.o"
rm -f "$HERE/mbxshim.o"

# GLTest.app -- a real app bundle with a CAEAGLLayer. Same no-linking rules as
# gles_fw: UIKit, QuartzCore, Foundation, OpenGLES and libobjc are all dlopen'd,
# so nothing here links anything but libSystem.
cc6 "$HERE/glapp.c" "$HERE/glapp.o"
link6 -execute "$HERE/GLTest" "$HERE/glapp.o"
rm -f "$HERE/glapp.o"

cc6 "$HERE/sblaunch.c" "$HERE/sblaunch.o"
link6 -execute "$HERE/sblaunch" "$HERE/sblaunch.o"
rm -f "$HERE/sblaunch.o"

APP="$HERE/GLTest.app"
rm -rf "$APP"
mkdir -p "$APP"
cp "$HERE/GLTest" "$APP/GLTest"
cp "$HERE/glapp-Info.plist" "$APP/Info.plist"
chmod 755 "$APP/GLTest"
# Ad-hoc sign it. The image's boot args disable enforcement, but installd and
# SpringBoard both walk the signature before anything is executed, and an
# unsigned binary is a different rejection from a bad one.
if command -v ldid >/dev/null; then
    ldid -S "$APP/GLTest"
fi

file "$HERE/gles_tri" "$HERE/gles_tex" "$HERE/gles_fw" "$HERE/MBXGLEngine" \
     "$APP/GLTest"
