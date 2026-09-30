#!/bin/bash
# Build the guest-side GLES binaries. See ../armv6-toolchain/README.md.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/../armv6-toolchain/armv6.sh"

# The MBXGLEngine.bundle replacement: one binary for every firmware, which reads
# its dispatch layout out of the running OpenGLES (gles_dispatch.c) and speaks
# the name-keyed wire of include/hw/arm/guest-services/gles-names.h.
# -bundle, and the install name does not matter: the framework dlopens it by
# path out of the .bundle directory.
python3 "$HERE/../gles-public/gligen.py" --check
python3 "$HERE/genstubs.py" "$HERE/gles_stubs.h" >/dev/null
cc6 "$HERE/mbxshim.c" "$HERE/mbxshim.o"
link6 -bundle "$HERE/MBXGLEngine" "$HERE/mbxshim.o"
# 4.x runs only signed code, amfi_allow_any_signature or not
if command -v ldid >/dev/null; then ldid -S "$HERE/MBXGLEngine"; fi
rm -f "$HERE/mbxshim.o"
# 3.0 (7A341) has the same engine ABI (MBXGLEngine.bundle, GLESGetEGLInterface, 821 slots), but its
# engine is a plain file (no shared cache) and its dyld refuses LC_DYLD_INFO_ONLY: the same source,
# legacy-linked. Signed like the others (an unsigned engine is killed at its first page on 3.0).
(export LEGACY_LINK=1
 cc6 "$HERE/mbxshim.c" "$HERE/mbxshim.o"
 link6 -bundle "$HERE/MBXGLEngine-30" "$HERE/mbxshim.o"
 if command -v ldid >/dev/null; then ldid -S "$HERE/MBXGLEngine-30"; fi
 rm -f "$HERE/mbxshim.o")

# 1.x/2.x have no engine bundle: OpenGLES itself is the driver, so the same core goes in as the
# framework binary under the firmware's own export names (build-gles2x.sh, gles2x.c).
bash "$HERE/build-gles2x.sh" "$HERE/OpenGLES-2x"
bash "$HERE/build-gles2x.sh" 1x "$HERE/OpenGLES-1x"

# GLTest.app -- a real app bundle with a CAEAGLLayer. UIKit, QuartzCore, Foundation,
# OpenGLES and libobjc are all dlopen'd, so nothing here links anything but libSystem:
# `ld -framework OpenGLES` against the 3.1.3 SDK is a hard error (its 2009 dylibs read
# as platform 'unknown', fatal for a framework though only a warning for -l).
cc6 "$HERE/glapp.c" "$HERE/glapp.o"
link6 -execute "$HERE/GLTest" "$HERE/glapp.o"
rm -f "$HERE/glapp.o"

cc6 "$HERE/sblaunch.c" "$HERE/sblaunch.o"
link6 -execute "$HERE/sblaunch" "$HERE/sblaunch.o"
rm -f "$HERE/sblaunch.o"
# SpringBoard rejects SBSLaunchApplicationWithIdentifier from a caller without
# com.apple.springboard.launchapplications, and reports the refusal only to the
# device console -- the call itself returns 1, exactly as it does for an app
# that is not installed. Signing with the entitlement is not optional.
if command -v ldid >/dev/null; then
    ldid "-S$HERE/sblaunch-entitlements.xml" "$HERE/sblaunch"
fi

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

file "$HERE/MBXGLEngine" "$HERE/OpenGLES-2x" "$HERE/OpenGLES-1x" \
     "$APP/GLTest"
