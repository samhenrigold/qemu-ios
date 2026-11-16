#!/bin/bash
# The development configure wrapper installs explicit cache launchers without hiding a caller's compiler override.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
if ! command -v ccache >/dev/null; then echo 'SKIP ccache not installed'; exit 0; fi
scratch="${LTM_SCRATCH_ROOT:-$HOME/Developer/ltm-scratch.noindex}"
mkdir -p "$scratch"
T="$(mktemp -d "$scratch/configure-cache-test.XXXXXX")"
trap 'rm -rf "$T"' EXIT
mkdir -p "$T/source/scripts" "$T/build" "$T/prefix/lib/pkgconfig"
cp "$ROOT/scripts/configure-patched-ffmpeg" "$T/source/scripts/"
touch "$T/prefix/lib/pkgconfig/libavcodec.pc" "$T/prefix/lib/pkgconfig/libavutil.pc"
cat > "$T/source/configure" <<'CONFIGURE'
#!/bin/sh
printf '%s\n' "$@" > "$CONFIG_ARGS"
CONFIGURE
chmod +x "$T/source/configure"
export CONFIG_ARGS="$T/args" FFMPEG_PREFIX="$T/prefix" CC=/usr/bin/clang CXX=/usr/bin/clang++
cd "$T/build"
"$T/source/scripts/configure-patched-ffmpeg" --cc=custom-compiler
grep -qx -- '--cc=ccache /usr/bin/clang' "$T/args"
grep -qx -- '--cxx=ccache /usr/bin/clang++' "$T/args"
[[ $(tail -1 "$T/args") = --cc=custom-compiler ]]
env -u CC -u CXX "$T/source/scripts/configure-patched-ffmpeg"
grep -qx -- "--cc=ccache $(xcrun -f clang)" "$T/args"
grep -qx -- "--cxx=ccache $(xcrun -f clang++)" "$T/args"
LTM_CCACHE=0 "$T/source/scripts/configure-patched-ffmpeg"
! grep -q -- '--cc=' "$T/args"
CC='ccache /usr/bin/clang' "$T/source/scripts/configure-patched-ffmpeg"
! grep -q 'ccache ccache' "$T/args"
echo 'PASS: configure uses explicit ccache launchers, preserves compiler overrides and supports opt-out'
