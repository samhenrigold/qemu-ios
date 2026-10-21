#!/bin/bash
# The GL bridge's ANGLE build calls EGL and GL only through the pointers it opens ANGLE for
# (hw/arm/gles-host-angle-gl.h): a name it referenced directly would bind to the system's OpenGL, which
# the CGL build beside it links.   angle-names.sh BUILD-DIR (configured with -Dltm_angle_prefix)
set -euo pipefail
OBJ="${1:?usage: angle-names.sh BUILD-DIR}/libqemu-arm-softmmu.a.p/hw_arm_gles-host-angle.c.o"
[ -f "$OBJ" ] || { echo "FAIL no $OBJ (build qemu-system-arm with -Dltm_angle_prefix)"; exit 1; }
direct=$(nm -u "$OBJ" | sed -n 's/^_\(e\{0,1\}gl[A-Z][A-Za-z0-9]*\)$/\1/p')
[ -z "$direct" ] || { echo "FAIL the ANGLE build calls these directly: $direct"; exit 1; }
echo "PASS the ANGLE build calls EGL and GL only through ANGLE's own entry points"
