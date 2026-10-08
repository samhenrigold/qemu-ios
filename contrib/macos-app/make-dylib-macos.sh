#!/bin/bash
# macOS sibling of contrib/ios-app/make-dylib.sh: relink the emulator as a
# dylib a Mac app links directly. Same trick -- take ninja's own link line for
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
for f in "$SRC/contrib/ios-app/qemu-ios-entry.c" \
         "$SRC/contrib/ios-app/qemu-ios-ui.c" \
         "$SRC/contrib/macos-app/qemu-macos-extras.c"; do
o="$(basename "${f%.c}").o"
"${CC_ARGV[@]}" -c "$f" -o "$o" \
    -I. -I.. -Iqapi -Itrace -Iui \
    -I"$SRC/contrib/ios-app" -I"$SRC/contrib/macos-app" \
    $DEP_CFLAGS \
    -iquote . -iquote "$SRC" -iquote "$SRC/include" \
    -iquote "$SRC/host/include/$HOST_CPU" -iquote "$SRC/host/include/generic" \
    -iquote "$SRC/tcg/$TCG_HOST" \
    -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE -std=gnu11 -O2 \
    -mmacosx-version-min=12.0
done

# Take ninja's own link line so this cannot drift from the real build.
ninja qemu-system-arm >/dev/null
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
printf '%q ' "${ARGS[@]}" > macos-link-dylib.sh
echo >> macos-link-dylib.sh

# Only what the app calls; everything else stays private.
cat > macos-exports.syms <<'SYMS'
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
_qemu_ios_agent_request
_qemu_ios_agent_cancel
_qemu_ios_agent_result
_qemu_ios_agent_free_result
_qemu_ios_agent_status
_qemu_ios_gles_contexts
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
_qemu_ios_snapshot_save2
_qemu_ios_snapshot_status
_qemu_ios_snapshot_resume
SYMS

bash macos-link-dylib.sh
codesign -f -s - "$OUT"
echo "built $OUT"
nm -gU "$OUT" | head -20
