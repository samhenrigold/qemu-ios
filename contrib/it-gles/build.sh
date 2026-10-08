#!/bin/bash
# Build the guest-side GLES binaries. See ../armv6-toolchain/README.md.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
[ $# -eq 0 ] || { echo "usage: build.sh" >&2; exit 1; }
. "$HERE/../armv6-toolchain/armv6.sh"

# The name table and the dispatch stubs the core (mbxshim.c, compiled into contrib/gles-public and gles1x.c) uses.
sh "$HERE/../gles-public/gligen.sh" >/dev/null
sh "$HERE/genstubs.sh" "$HERE/gles_stubs.h"

# 1.x (the iPod touch 1G): OpenGLES itself is the driver, with no EAGL and the old ObjC runtime, so the same core
# goes in as the framework binary under 1.x's own export names (build-gles1x.sh, gles1x.c). 2.x-5.x take the
# one front end, contrib/gles-public.
bash "$HERE/build-gles1x.sh" "$HERE/OpenGLES-1x"

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

file "$HERE/OpenGLES-1x" "$APP/GLTest"
