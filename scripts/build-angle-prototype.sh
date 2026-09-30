#!/bin/bash
# Isolated, pinned ANGLE Metal evaluation. Requires git, Python 3, GN's
# downloaded tools, Ninja and Xcode. Downloads/builds roughly 10 GB.
set -euo pipefail
DEST="${1:?usage: build-angle-prototype.sh NEW-DIRECTORY}"
[ ! -e "$DEST" ] || { echo "use a new directory: $DEST" >&2; exit 1; }
mkdir -p "$DEST"
DEST="$(cd "$DEST" && pwd)"
fetch() {
    git init "$1"
    git -C "$1" remote add origin "$2"
    git -C "$1" fetch --depth 1 origin "$3"
    git -C "$1" checkout --detach FETCH_HEAD
}
fetch "$DEST/angle" https://github.com/google/angle.git 8cd050f07ebd65cce269fc92e36699fc03b20ac3
fetch "$DEST/depot_tools" https://chromium.googlesource.com/chromium/tools/depot_tools.git 9b264039190fa270f3fc44779b4e26b43008ba41
export PATH="$DEST/depot_tools:$PATH" DEPOT_TOOLS_UPDATE=0
cat > "$DEST/.gclient" <<'EOF'
solutions=[{"name":"angle","url":"https://github.com/google/angle.git","managed":False,"custom_deps":{},"custom_vars":{"checkout_angle_cl_deps":False,"checkout_angle_dawn_deps":False,"checkout_angle_internal":False,"checkout_angle_restricted_traces":False,"checkout_angle_mesa":False,"download_remoteexec_cfg":False}}]
EOF
cd "$DEST"
gclient sync --shallow --no-history -j6
cd angle
buildtools/mac/gn gen out/ltm-metal --args='is_debug=false is_component_build=true angle_enable_metal=true angle_enable_gl=false angle_enable_vulkan=false angle_enable_null=false angle_build_tests=false angle_build_all=false use_remoteexec=false use_siso=false clang_use_chrome_plugins=false use_custom_libcxx=false mac_deployment_target="14.0"'
ninja -C out/ltm-metal -j"${ANGLE_JOBS:-8}" libEGL libGLESv2
printf '%s\n' "$DEST/angle/out/ltm-metal"
