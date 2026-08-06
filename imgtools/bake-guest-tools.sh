#!/bin/sh
#
# Bake the guest-side install tools into a NAND page image, so an app install
# needs no ssh at all: the GL engine replacement and the launcher/placeholder
# helpers are already on the device, and a marker in the AFC jail lets the host
# app detect a baked image without probing over ssh.
#
# This is what produced nand-ultimate (= nand-appsync3 + these tools). Run it
# through editimg.py, which sets $MNT to the mounted guest volume:
#
#     cp -Rc <image> <image>-baked
#     imgtools/editimg.py --nand <image>-baked --blocks 1835008 \
#         --script imgtools/bake-guest-tools.sh
#     imgtools/setowner.py --nand <image>-baked \
#         /usr/local/bin/sblaunch:0:0:755 \
#         /usr/local/bin/sbdlicon:0:0:755 \
#         /System/Library/Frameworks/OpenGLES.framework/MBXGLEngine.bundle/MBXGLEngine:0:0:755
#
# --blocks is the volume size: 1835008 for the 7 GiB images, 128000 for 500 MB.
#
# The setowner.py step is NOT optional: editimg mounts `noowners` as an ordinary
# user, so files land 99:99. iOS 3.1.3 will not exec a non-root-owned binary in
# /usr/local/bin, and the GL engine must be root-owned to load — skip setowner
# and the device installs apps that then wedge, exactly what baking prevents.
# (See contrib/it-pasteboard/README.md for the full ownership rationale.)
#
# Marker versioning: bump the marker name when the baked tool set changes, so a
# host app keyed on it does not assume tools an older image lacks.
set -e
SRC="$(cd "$(dirname "$0")/.." && pwd)"
GLES="$SRC/contrib/it-gles"
INST="$SRC/contrib/it-instprogress"
FRAMEWORK="$MNT/System/Library/Frameworks/OpenGLES.framework/MBXGLEngine.bundle"

# 1. The GL engine replacement, stock preserved. Without it a GL app drives the
#    unemulated PowerVR MBX and wedges the whole device on first launch.
[ -f "$GLES/MBXGLEngine" ] || { echo "no $GLES/MBXGLEngine (run contrib/it-gles/build.sh)" >&2; exit 1; }
mkdir -p "$FRAMEWORK"
[ -f "$FRAMEWORK/MBXGLEngine.stock" ] || cp -n "$FRAMEWORK/MBXGLEngine" "$FRAMEWORK/MBXGLEngine.stock" 2>/dev/null || true
cp "$GLES/MBXGLEngine" "$FRAMEWORK/MBXGLEngine"
chmod 755 "$FRAMEWORK/MBXGLEngine"

# 2. The launcher (SBSLaunchApplicationWithIdentifier) and the placeholder-icon
#    helper, both of which otherwise have to be scp'd and chmod'd per install.
mkdir -p "$MNT/usr/local/bin"
cp "$GLES/sblaunch" "$MNT/usr/local/bin/sblaunch"
chmod 755 "$MNT/usr/local/bin/sblaunch"
if [ -f "$INST/sbdlicon" ]; then
    cp "$INST/sbdlicon" "$MNT/usr/local/bin/sbdlicon"
    chmod 755 "$MNT/usr/local/bin/sbdlicon"
fi

# 3. The marker, inside the AFC jail (/var/mobile/Media) so the host app can
#    stat it over AFC — no ssh — and take the fully in-process install path.
mkdir -p "$MNT/var/mobile/Media"
echo "v1" > "$MNT/var/mobile/Media/.lt-guest-tools-v1"

echo "baked: MBXGLEngine, sblaunch$([ -f "$INST/sbdlicon" ] && echo ', sbdlicon'), marker .lt-guest-tools-v1"
echo "NEXT: run imgtools/setowner.py (see header) or the tools stay uid 99 and will not run"
