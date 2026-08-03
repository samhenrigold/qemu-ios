#!/bin/bash
# Build the guest-side GLES test binaries. See ../armv6-toolchain/README.md.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/../armv6-toolchain/armv6.sh"

for t in gles_tri gles_tex; do
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

file "$HERE/gles_tri" "$HERE/gles_tex" "$HERE/gles_fw" "$HERE/MBXGLEngine"
