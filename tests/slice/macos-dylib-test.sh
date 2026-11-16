#!/bin/bash
# The production dylib generator over a tiny real Mach-O build: no-op builds preserve signed outputs; source,
# header, compiler-environment and executable changes invalidate the correct work. No emulator or SDK fixtures.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
scratch="${LTM_SCRATCH_ROOT:-$HOME/Developer/ltm-scratch.noindex}"
mkdir -p "$scratch"
T="$(mktemp -d "$scratch/macos-dylib-test.XXXXXX")"
trap 'rm -rf "$T"' EXIT
SRC="$T/source \$: with spaces"
BUILD="$T/build with spaces"
mkdir -p "$SRC/contrib/macos-app" "$SRC/contrib/ios-app" "$BUILD/meson-info" "$T/bin"
cp "$ROOT/contrib/macos-app/make-dylib-macos.sh" "$SRC/contrib/macos-app/"
printf '#define INPUT_VALUE 1\n' > "$SRC/contrib/ios-app/build-input.h"
{
    printf '#include "build-input.h"\n#ifndef ENV_CHANGE\n#define ENV_CHANGE 0\n#endif\nint header_value = INPUT_VALUE;\nint environment_value = ENV_CHANGE;\n'
    sed -n '/^_qemu_ios_/s/^_\(.*\)$/void \1(void) {}/p' "$ROOT/contrib/macos-app/make-dylib-macos.sh"
} > "$SRC/contrib/ios-app/qemu-ios-entry.c"
printf 'int app_ui;\n' > "$SRC/contrib/ios-app/qemu-ios-ui.c"
printf 'int app_extras;\n' > "$SRC/contrib/macos-app/qemu-macos-extras.c"
printf '#!/bin/sh\nexit 0\n' > "$T/bin/pkg-config"
chmod +x "$T/bin/pkg-config"
# The main executable's real link line is what the generator transforms into a dylib link.
printf 'int main(void) { return 0; }\n' > "$BUILD/main.c"
/usr/bin/clang -c "$BUILD/main.c" -o "$BUILD/system_main.c.o"
cat > "$BUILD/build.ninja" <<'NINJA'
rule main_cc
  command = /usr/bin/clang -MMD -MF $out.d -c $in -o $out
  depfile = $out.d
  deps = gcc
rule executable
  command = /usr/bin/clang system_main.c.o -o $out
rule copy
  command = cp $in $out
build system_main.c.o: main_cc main.c
build qemu-system-arm-unsigned: executable system_main.c.o
build qemu-system-arm: copy qemu-system-arm-unsigned
NINJA
printf '{"host":{"cpu_family":"aarch64"}}\n' > "$BUILD/meson-info/intro-machines.json"
printf '{"host":{"c":{"exelist":["/usr/bin/clang"]}}}\n' > "$BUILD/meson-info/intro-compilers.json"
export PATH="$T/bin:$PATH"
# Test-only ccache keeps the suite independent of another developer's cache contents and doesn't clear their cache.
export CCACHE_DIR="$T/ccache"
build() { bash "$SRC/contrib/macos-app/make-dylib-macos.sh" "$BUILD" > "$T/output" 2>&1 || { cat "$T/output"; exit 1; }; }
stamp() { python3 -I -c 'import os,sys; print(os.stat(sys.argv[1]).st_mtime_ns)' "$BUILD/$1"; }
fail() { echo "FAIL: $*"; cat "$T/output"; exit 1; }
build
codesign --verify "$BUILD/libqemu-arm.dylib"
main_deps="$(cksum "$BUILD/.ninja_deps")"
first="$(stamp libqemu-arm.dylib)"; entry="$(stamp qemu-ios-entry.o)"; ui="$(stamp qemu-ios-ui.o)"
build
[[ "$(stamp libqemu-arm.dylib)" = "$first" ]] || fail 'no-op relinked or signed the library'
grep -q 'no work to do' "$T/output" || fail 'no-op did not reach Ninja'
sleep 0.1
printf '#define INPUT_VALUE 2\n' > "$SRC/contrib/ios-app/build-input.h"
build
[[ "$(stamp qemu-ios-entry.o)" != "$entry" && "$(stamp qemu-ios-ui.o)" = "$ui" ]] || fail 'header dependencies not tracked precisely'
[[ "$(stamp libqemu-arm.dylib)" != "$first" ]] || fail 'changed object did not relink'
first="$(stamp libqemu-arm.dylib)"; ui="$(stamp qemu-ios-ui.o)"
CCC_OVERRIDE_OPTIONS=+-DENV_CHANGE=7 build
[[ "$(stamp libqemu-arm.dylib)" != "$first" && "$(stamp qemu-ios-ui.o)" != "$ui" ]] || fail 'compiler environment did not rebuild'
# Both variables are made public only in the fixture's export list for inspecting actual compiled data.
sed -i '' '/^_qemu_ios_main$/a\
_header_value\
_environment_value\
' "$SRC/contrib/macos-app/make-dylib-macos.sh"
CCC_OVERRIDE_OPTIONS=+-DENV_CHANGE=7 build
python3 -I - "$BUILD/libqemu-arm.dylib" <<'PY'
import ctypes, sys
lib = ctypes.CDLL(sys.argv[1])
assert ctypes.c_int.in_dll(lib, 'header_value').value == 2
assert ctypes.c_int.in_dll(lib, 'environment_value').value == 7, 'ccache reused an object with stale compiler flags'
PY
[[ "$(cksum "$BUILD/.ninja_deps")" = "$main_deps" ]] || fail 'dylib subgraph modified main Ninja dependency state'
ui="$(stamp qemu-ios-ui.o)"; first="$(stamp libqemu-arm.dylib)"
sleep 0.1
touch "$BUILD/system_main.c.o"
CCC_OVERRIDE_OPTIONS=+-DENV_CHANGE=7 build
[[ "$(stamp libqemu-arm.dylib)" != "$first" && "$(stamp qemu-ios-ui.o)" = "$ui" ]] || fail 'main objects did not relink without redundant app compilation'
# Cross-compilation must keep Meson's compiler argv, including -arch, instead of using the host compiler alone.
printf '{"host":{"cpu_family":"x86_64"}}\n' > "$BUILD/meson-info/intro-machines.json"
printf '{"host":{"c":{"exelist":["/usr/bin/clang","-arch","x86_64"]}}}\n' > "$BUILD/meson-info/intro-compilers.json"
/usr/bin/clang -arch x86_64 -c "$BUILD/main.c" -o "$BUILD/system_main.c.o"
sed -i '' 's|/usr/bin/clang |/usr/bin/clang -arch x86_64 |' "$BUILD/build.ninja"
CCC_OVERRIDE_OPTIONS=+-DENV_CHANGE=7 build
lipo -verify_arch x86_64 "$BUILD/libqemu-arm.dylib"
codesign --verify "$BUILD/libqemu-arm.dylib"
echo 'PASS: real dylib build is incremental, signed, dependency-aware and safe with compiler environment caching'
