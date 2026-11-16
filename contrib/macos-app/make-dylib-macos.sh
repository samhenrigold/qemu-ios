#!/bin/bash
# Relink the emulator as a dylib a Mac app links directly: take ninja's own link line for
# qemu-system-arm, swap system/main.c (whose main() wants the main thread for
# CFRunLoopRun) for qemu-ios-entry.c, and emit a dylib exporting only the app
# ABI. The app calls qemu_ios_main() on a background thread.
#
#     make-dylib-macos.sh [build-dir]     # default build-min12b
set -eu

SRC="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="${1:-$SRC/build-min12b}"
LIBNAME="libqemu-arm.dylib"
OUT="$BUILD/$LIBNAME"

cd "$BUILD"
BUILD="$PWD"
OUT="$BUILD/$LIBNAME"
TMP="$(mktemp -d "$BUILD/.macos-dylib.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT
# Preserve mtimes when generated commands and exports are unchanged: Ninja can then do a true no-op build.
publish() { cmp -s "$TMP/$1" "$1" || mv "$TMP/$1" "$1"; }
ninja_path() {
    local value="$1"
    value="${value//\$/\$\$}"; value="${value// /\$ }"; value="${value//:/\$:}"
    printf '%s' "$value"
}
ninja_command() {
    local value
    printf -v value '%q ' "$@"
    printf '%s' "${value//\$/\$\$}"
}
# Main objects and generated headers must be current before compiling the app-facing translation units.
ninja qemu-system-arm >/dev/null

# Compile the app-facing pieces with the same include set the emulator uses.
# glib/pixman come from Homebrew, exactly as the meson build found them.
DEP_CFLAGS="$(pkg-config --cflags glib-2.0 pixman-1)"
# Use the build's own compiler and host CPU, so a cross-compiled x86_64 build
# (the Intel slice of a universal app) compiles these for the same target.
HOST_CPU="$(jq -r .host.cpu_family meson-info/intro-machines.json)"
eval "CC_ARGV=($(jq -r '.host.c.exelist | @sh' meson-info/intro-compilers.json))"
case "$HOST_CPU" in
    aarch64) TCG_HOST=aarch64 ;;
    x86_64) TCG_HOST=i386 ;;
    *) echo "unsupported host CPU: $HOST_CPU" >&2; exit 1 ;;
esac
# Meson may name an absolute compiler, which a PATH masquerade cannot intercept.
if [ "${LTM_CCACHE:-1}" != 0 ] && command -v ccache >/dev/null && [ "$(basename "${CC_ARGV[0]}")" != ccache ]; then
    CC_ARGV=(ccache "${CC_ARGV[@]}")
fi
# Preserve __FILE__ and macro-prefix-map behavior exactly; share storage without rewriting source arguments.
export CCACHE_BASEDIR=""
ENV_ARGS=(env)
for variable in CCACHE_BASEDIR CCC_OVERRIDE_OPTIONS DEVELOPER_DIR SDKROOT MACOSX_DEPLOYMENT_TARGET CPATH C_INCLUDE_PATH CPLUS_INCLUDE_PATH; do
    if [[ ${!variable+x} ]]; then ENV_ARGS+=("$variable=${!variable}"); fi
done
# Include the selected compiler and SDK identity in Ninja's command hash as well as the real command-line flags.
TOOLCHAIN="$( { "${CC_ARGV[@]}" --version; xcrun --sdk macosx --show-sdk-build-version; } | shasum -a 256 | cut -d' ' -f1)"
# Clang's CCC_OVERRIDE_OPTIONS changes code generation outside its argv. Give that environment its own ccache
# namespace too, so an incremental rebuild cannot fetch an object compiled under different hidden flags.
CACHE_ENV="$(printf '%s\0' "${ENV_ARGS[@]}" "$TOOLCHAIN" | shasum -a 256 | cut -d' ' -f1)"
ENV_ARGS+=("LTM_DYLIB_TOOLCHAIN=$TOOLCHAIN" "CCACHE_NAMESPACE=${CCACHE_NAMESPACE:-}qemu-dylib-$CACHE_ENV")
# A separate log/deps directory prevents Ninja's subgraph compaction from discarding the main build's history.
cat > "$TMP/macos-dylib.ninja" <<'NINJA'
builddir = .macos-dylib
rule macos_cc
  command = $cmd
  depfile = $out.d
  deps = gcc
rule macos_link
  command = $cmd
NINJA
for f in "$SRC/contrib/ios-app/qemu-ios-entry.c" \
         "$SRC/contrib/ios-app/qemu-ios-ui.c" \
         "$SRC/contrib/macos-app/qemu-macos-extras.c"; do
    o="$(basename "${f%.c}").o"
    printf 'build %s: macos_cc %s\n  cmd = ' "$o" "$(ninja_path "$f")" >> "$TMP/macos-dylib.ninja"
    # pkg-config emits shell words, as in the executable's build. The explicit source/header paths stay quoted.
    ninja_command "${ENV_ARGS[@]}" "${CC_ARGV[@]}" -c "$f" -o "$o" -MMD -MF "$o.d" -MT "$o" \
        -I. -I.. -Iqapi -Itrace -Iui -I"$SRC/contrib/ios-app" -I"$SRC/contrib/macos-app" \
        $DEP_CFLAGS -iquote . -iquote "$SRC" -iquote "$SRC/include" \
        -iquote "$SRC/host/include/$HOST_CPU" -iquote "$SRC/host/include/generic" -iquote "$SRC/tcg/$TCG_HOST" \
        -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE -std=gnu11 -O2 \
        -mmacosx-version-min=12.0 >> "$TMP/macos-dylib.ninja"
    echo >> "$TMP/macos-dylib.ninja"
done

# Take ninja's own link line so this cannot drift from the real build.
eval "LINK=($(ninja -t commands qemu-system-arm-unsigned | tail -1))"
ARGS=()
skip=0
for a in "${LINK[@]}"; do
    if [ $skip = 1 ]; then skip=0; continue; fi
    case "$a" in
        -o) skip=1 ;;                                   # drop the executable name
        *system_main.c.o)                               # main() -> qemu_ios_main(); frames out, touches in;
            ARGS+=(qemu-ios-entry.o qemu-ios-ui.o qemu-macos-extras.o) ;;   # keys, pinch, machine controls
        @*) ;;   # @block.syms / @qemu.syms export an executable's symbols; a dylib exports our entry points
        *) ARGS+=("$a") ;;
    esac
done
ARGS+=(-dynamiclib -o "$OUT" -install_name "@rpath/$LIBNAME"
      "-Wl,-exported_symbols_list,$BUILD/macos-exports.syms" -Wl,-undefined,dynamic_lookup)
printf '%q ' "${ARGS[@]}" > "$TMP/macos-link-dylib.sh"
echo >> "$TMP/macos-link-dylib.sh"
publish macos-link-dylib.sh

# Only what the app calls; everything else stays private.
cat > "$TMP/macos-exports.syms" <<'SYMS'
_qemu_ios_main
_qemu_ios_device_info
_qemu_ios_device_info_at
_qemu_ios_ui_attach
_qemu_ios_ui_frame
_qemu_ios_ui_touch
_qemu_ios_ui_modem_set
_qemu_ios_ui_modem_status
_qemu_ios_ui_modem_free
_qemu_ios_ui_touch2
_qemu_ios_ui_button
_qemu_ios_ui_input_sequence
_qemu_ios_ui_input_sequence_status
_qemu_ios_ui_input_sequence_cancel
_qemu_ios_ui_key_mac
_qemu_ios_ui_rotate
_qemu_ios_ui_shake
_qemu_ios_ui_attitude
_qemu_ios_ui_battery
_qemu_ios_ui_usb_connection
_qemu_ios_ui_compass
_qemu_ios_ui_usb_charger
_qemu_ios_ui_orientation
_qemu_ios_audio_capture_start
_qemu_ios_audio_capture_read
_qemu_ios_audio_capture_stop
_qemu_ios_ui_paste
_qemu_ios_ui_guest_pasteboard
_qemu_ios_agent_request
_qemu_ios_agent_cancel
_qemu_ios_agent_result
_qemu_ios_agent_free_result
_qemu_ios_agent_status
_qemu_ios_agent_report
_qemu_ios_gles_contexts
_qemu_ios_gles_refusals
_qemu_ios_guest_package_report
_qemu_ios_gles_protocol
_qemu_ios_build_id
_qemu_ios_api_version
_qemu_ios_ui_pause
_qemu_ios_ui_resume
_qemu_ios_ui_reset
_qemu_ios_ui_powerdown
_qemu_ios_ui_quit
_qemu_ios_ui_shutdown
_qemu_ios_ui_hardware_keyboard
_qemu_ios_ui_net_restrict
_qemu_ios_ui_net_lan
_qemu_ios_ui_ready
_qemu_ios_ui_storage_failed
_qemu_ios_ui_guest_shutdown_confirmed
_qemu_ios_ui_display_sleeping
_qemu_ios_ui_backlight_level
_qemu_ios_ui_vibrator
_qemu_ios_ui_touch_waiting
_qemu_ios_snapshot_save2
_qemu_ios_snapshot_status
_qemu_ios_snapshot_resume
SYMS

publish macos-exports.syms
{
    printf 'build %s: macos_link qemu-ios-entry.o qemu-ios-ui.o qemu-macos-extras.o | qemu-system-arm-unsigned macos-exports.syms macos-link-dylib.sh\n  cmd = ' "$LIBNAME"
    ninja_command "${ENV_ARGS[@]}" bash macos-link-dylib.sh
    printf ' && '
    ninja_command codesign -f -s - "$OUT"
    printf '\ndefault %s\n' "$LIBNAME"
} >> "$TMP/macos-dylib.ninja"
publish macos-dylib.ninja
ninja -f macos-dylib.ninja
echo "ready $OUT"
nm -gU "$OUT" | head -20
