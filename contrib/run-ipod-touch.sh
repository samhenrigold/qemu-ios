#!/bin/bash
#
# Run the emulated iPod touch 2G on iOS 3.1.3, in a window, interactively.
#
#     run-ipod-touch.sh                 # keeps state across runs
#     run-ipod-touch.sh --fresh         # wipe back to a clean device
#     run-ipod-touch.sh --small         # the original 500 MB volume instead of 7 GiB
#     run-ipod-touch.sh --net           # emulated WiFi with host networking
#     run-ipod-touch.sh --apps          # open the kernel's code-signing gate
#     run-ipod-touch.sh --keyboard      # type on the guest with the host keyboard
#     run-ipod-touch.sh --usb           # expose the device to usbmuxd over USB
#     run-ipod-touch.sh --appsync       # install and run third-party apps (see below)
#     run-ipod-touch.sh --sound         # send the speaker to the host's audio output
#     run-ipod-touch.sh --help          # the full list, including diagnostics
#
# Flags combine: `run-ipod-touch.sh --fresh --big --net`.
#
# CLIPBOARD, no flag needed: copy on the Mac, then Edit > Paste Text to Guest
# (Cmd+Ctrl+V) and tap Paste in the guest. Copying inside the guest goes back to
# the Mac clipboard by itself. See contrib/it-pasteboard/README.md.
#
# TO INSTALL AND RUN APPS -- two commands, IN THIS ORDER. --appsync starts
# usbmuxd itself, before QEMU, because QEMU dials OUT to it and gives up for the
# rest of the boot if nothing is listening. Full walkthrough in
# docs/ipod-touch-2g-setup.md:
#
#     run-ipod-touch.sh --appsync              # start this first, wait for the UI
#     imgtools/install-ipa.sh some.ipa
#
# State persists by default. Writes land in ios3/nandrw as a NAND overlay; the
# base image is never modified, so --fresh always gets you back to clean.
#
# NOTE ON --fresh: a persisted overlay shadows base-image fixes made after it was
# created - the timezone is the one that bites - so if the clock or a setting
# looks stale, --fresh clears it.
#
# The window takes roughly 40 seconds to reach the lock screen. Drag the
# slide-to-unlock knob with the mouse.
#
# Everything this needs lives under $IPOD_FILES (default
# ~/Developer/qemu-ios-files): the release images, unpacked.
set -u

# Where the images live: bootrom_240_4, nor_7E18.bin, iBoot.bin and the NAND
# directories from the release. Override with IPOD_FILES=<dir>.
F="${IPOD_FILES:-$HOME/Developer/qemu-ios-files}"
HERE="$F/ios3"
QEMU="${QEMU:-$(cd "$(dirname "$0")/.." && pwd)/build/qemu-system-arm}"
NAND="${NAND:-$F/nand-grow7g}"
# One overlay PER BASE IMAGE, never shared. A NAND overlay records writes by
# block against the layout of the base it was made from; point it at a
# different base and it shadows blocks that mean something else there. That has
# already produced a hang at the Apple logo with no kernel error, read at the
# time as "the patch broke boot". --fresh is applied after parsing, so it wipes
# whichever overlay the other flags actually selected.
OVL="$HERE/nandrw"
FRESH=0
APPSYNC=0
QMP_PORT="${QMP_PORT:-4444}"
SESSION="$F/apps/work/session.env"

# Pick PREF if it is free, otherwise any free port. Several emulators run on
# this machine at once and every port here used to be a hardcoded constant --
# which silently cross-connected two instances, and because the UDID is derived
# from our shared NOR they were indistinguishable to any client. An install went
# to whichever device answered first.
#
# KNOWN LIMITATION (TOCTOU): this binds a port to test it, closes it, and the
# real bind happens later, so two --appsync launches racing each other can still
# pick the same port. The window is kept small by picking each port immediately
# before the process that claims it. Not worth a lockfile yet; if you ever see
# two instances collide, that is this.
pick_port() {
    # In the app bundle this is ipod-helper, because a clean macOS has no
    # python3 -- /usr/bin/python3 is a Command Line Tools shim that pops an
    # install dialog. Run from a source tree the helper may not be built, and
    # a developer machine has python3, so that path still works.
    if [ -x "${IT_HELPER:-}" ]; then
        "$IT_HELPER" pick-port "$1"
        return
    fi
    python3 - "$1" <<'PY'
import socket, sys
for p in (int(sys.argv[1]), 0):
    s = socket.socket()
    try:
        s.bind(("127.0.0.1", p))
        print(s.getsockname()[1])
        break
    except OSError:
        pass
    finally:
        s.close()
PY
}

# QEMU dials OUT to usbmuxd, so usbmuxd must already be listening when the
# guest's USB core comes up. If it is not, QEMU logs "no host bridge at ..." and
# does not reconnect for the rest of that boot -- so starting usbmuxd afterwards
# cannot work, however long you wait. That ordering is the whole reason this
# lives here rather than in install-app.sh.
ensure_usbmuxd() {
    local root="${MUXD_ROOT:-$HOME/Developer/usbmuxd-qemu}"
    local muxd="${MUXD:-$root/usbmuxd/src/usbmuxd}"
    mkdir -p "$(dirname "$SESSION")"

    if [ ! -x "$muxd" ]; then
        echo "run-ipod-touch.sh: no usbmuxd at $muxd -- app install will not work." >&2
        echo "  build it, or set MUXD=<path>. Continuing without USB." >&2
        return
    fi

    QEMU_ADDR="127.0.0.1:$(pick_port 1235)"
    SOCK="127.0.0.1:$(pick_port 27015)"

    USBMUXD_QEMU_ADDR="$QEMU_ADDR" USBMUXD_QEMU_DELAY=100 \
        "$muxd" -f -v -v -S "$SOCK" -P NONE \
        -C "${MUXD_CONF:-$root/run/conf}" \
        >"$F/apps/work/usbmuxd.log" 2>&1 &
    MUXD_PID=$!
    sleep 1
    if ! kill -0 "$MUXD_PID" 2>/dev/null; then
        echo "run-ipod-touch.sh: usbmuxd died immediately; see apps/work/usbmuxd.log" >&2
        MUXD_PID=""
        return
    fi

    export IT_USB_TCP="$QEMU_ADDR"
    cat >"$SESSION" <<EOF
# written by run-ipod-touch.sh --appsync; read by apps/install-app.sh
SOCK=$SOCK
QEMU_ADDR=$QEMU_ADDR
MUXD_PID=$MUXD_PID
NAND=$NAND
OVL=$OVL
EOF
    echo "--- usbmuxd up (pid $MUXD_PID): clients $SOCK, guest $QEMU_ADDR"
    echo "--- install apps with:  imgtools/install-ipa.sh <file.ipa>"
}

# Only ever kill the usbmuxd THIS script started. A pkill pattern here would
# hit other people's runs and the host's own Apple usbmuxd.
cleanup() {
    if [ -n "${QEMU_PID:-}" ] && kill -0 "$QEMU_PID" 2>/dev/null; then
        kill "$QEMU_PID" 2>/dev/null || true
    fi
    if [ -n "${MUXD_PID:-}" ] && kill -0 "$MUXD_PID" 2>/dev/null; then
        kill "$MUXD_PID" 2>/dev/null || true
    fi
    rm -f "$SESSION"
}
MACHOPTS=""
NETOPTS=()
# NB: -audio driver=X, not -audiodev. A card created in C code reads the
# DEFAULT audiodev list, which only -audio populates.
AUDIOOPTS=()

usage() {
    sed -n '3,36p' "$0" | sed 's/^# \{0,1\}//'
    cat <<'EOF'

Diagnostics (all off by default, all safe to combine):
  --trace-usb        every USB PHY access and the core state on each NAK
  --trace-audio      AMC + I2S register access, and codec I2C traffic
  --trace-touch      multitouch SPI commands and frame delivery
  --trace-aes        which image each GID key request decrypts
  --amc-state        answer the AMC command/status handshake (experimental)
  --sound            host audio out (coreaudio); --sound=FILE records a wav
  --tone SECONDS     inject a test sine into the I2S FIFO, to check the host
                     output path end to end without involving the guest
  --i2c-nak          make absent I2C devices NAK instead of reading 0xff
  --volbtn-legacy    restore the old (wrong) volume-button polarity

The volume buttons are active low and now rest high by default, which is what
stopped the volume HUD appearing and flickering on its own from boot.
--volbtn-legacy puts the old resting level back, for bisecting only.

--usb needs usbmuxd running FIRST, or the device dials out to nothing:
  cd ~/Developer/usbmuxd-qemu && USBMUXD_QEMU_ADDR=127.0.0.1:1235 \
    USBMUXD_QEMU_DELAY=100 ./usbmuxd/src/usbmuxd -f -v -v \
    -S 127.0.0.1:27015 -P NONE -C ./run/conf
then point clients at it: USBMUXD_SOCKET_ADDRESS=127.0.0.1:27015 ideviceinfo

Stopping a --appsync run: close the window, Ctrl-C, or a plain `kill <pid>`.
All three run the cleanup that reaps usbmuxd and removes apps/work/session.env.
Do NOT use `kill -9`: SIGKILL cannot be caught, so no cleanup runs, QEMU is
orphaned and the usbmuxd is leaked holding its socket.

Environment overrides:
  QEMU=<path>        emulator binary
  NAND=<path>        base NAND image
  MUXD=<path>        usbmuxd binary (--appsync starts it for you)
  QMP_PORT=<n>       preferred QMP port; taken only if free
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
    --fresh)    FRESH=1 ;;
    --small)    NAND="$F/nand-7e18-final"; OVL="$HERE/nandrw-small" ;;
    --big)      : ;;   # 7 GiB is now the default; kept so old commands still work
    --net)      MACHOPTS="$MACHOPTS,wifi=on"
                NETOPTS=(-netdev user,id=wifi0) ;;
    --apps)     export IT_BOOT_ARGS="amfi_allow_any_signature=1 cs_enforcement_disable=1" ;;
    # Everything needed to install and run third-party apps, in one flag: the
    # AppSync-patched image, the kernel gate, and USB. See apps/README.md.
    # Also starts usbmuxd -- see the ordering note at ensure_usbmuxd below.
    # NAND is only defaulted, not forced: the app stages whichever image it
    # actually shipped (build-app.sh --nand) and exports NAND, and clobbering
    # that pointed QEMU at a directory the app never unpacked.
    --appsync)  NAND="${NAND:-$F/nand-appsync3}"
                OVL="$HERE/nandrw-appsync"
                # One overlay per base image, still. The name above is kept
                # verbatim for the default so existing devices keep their apps
                # and settings; anything else gets its own, because an overlay
                # replayed onto a different base shadows blocks that mean
                # something else there.
                [ "$(basename "$NAND")" = "nand-appsync3" ] ||
                    OVL="$HERE/nandrw-$(basename "$NAND")"
                APPSYNC=1
                export IT_BOOT_ARGS="amfi_allow_any_signature=1 cs_enforcement_disable=1"
                export IT_BOOT_ARGS_DELAY_MS=1500
                export IT_BOOT_ARGS_REPEAT=200
                export IT_BOOT_ARGS_INTERVAL_MS=250
                export IT_OSK=1 ;;
    --keyboard) export IT_OSK=1 ;;
    --usb)      export IT_USB_TCP="${IT_USB_TCP:-127.0.0.1:1235}" ;;
    --trace-usb)   export IT_USB_TRACE=1 ;;
    --trace-audio) export IT_AMC_TRACE=1 IT_CODEC_TRACE=1 ;;
    --trace-touch) export MT_TRACE=1 ;;
    --trace-aes)   export IT_AES_DEBUG=1 ;;
    --amc-state)   export IT_AMC_STATE=1 ;;
    # out.buffer-count=16 is load-bearing, not tuning. QEMU's downstream
    # capacity is buffer-count x 512 frames -- 46 ms at the default 4 -- and a
    # stalled main loop eats exactly that. The device-side prebuffer FILLS that
    # capacity but cannot CREATE it, so both are needed: measured, defaults gave
    # 3 of 3 clips damaged and 18 starved callbacks; both together gave 0 and 0.
    --sound)       AUDIOOPTS=(-audio driver=coreaudio,out.buffer-count=16) ;;
    --sound=*)     AUDIOOPTS=(-audio "driver=wav,path=${1#--sound=}") ;;
    --tone)        shift; export IT_I2S_TONE="$1" IT_I2S_DEBUG=1
                   [ ${#AUDIOOPTS[@]} -gt 0 ] || AUDIOOPTS=(-audio driver=coreaudio,out.buffer-count=16) ;;
    --i2c-nak)     export IT_I2C_NAK=1 ;;
    --volbtn-legacy) export IT_VOLBTN_LEGACY=1 ;;
    --help|-h)  usage; exit 0 ;;
    -*)         echo "run-ipod-touch.sh: unknown flag $1 (try --help)" >&2; exit 2 ;;
    *)          break ;;
    esac
    shift
done
# Device > Erase All Content and Settings drops this marker and quits, because
# the overlay's pages are open and being written to while the machine runs and
# deleting them underneath it corrupts whatever is in flight. Acting on it here,
# before QEMU starts, is the only point where the overlay is nobody's.
if [ -e "$OVL/.erase-on-next-start" ]; then
    echo "--- erasing $OVL (requested from the Device menu)"
    FRESH=1
fi
[ "$FRESH" = 1 ] && rm -rf "$OVL"
mkdir -p "$OVL"

# --- is your saved state hiding a newer base image? ------------------------
# An overlay records writes BY BLOCK, so it does not just add to the base -- it
# shadows any block the base changed afterwards. Bake a new file into the image
# and an old overlay serves the old version of the directory it lives in, so the
# file is simply not there as far as the guest is concerned. That is not
# theoretical: it is exactly what hid /usr/local/bin/it_pbd (the clipboard
# daemon) after it was added to nand-grow7g -- the device booted perfectly and
# the feature was silently absent.
#
# The stamp records when this overlay started. If any page of the base is newer,
# say so, because the symptom otherwise is "a feature the release notes promise
# does nothing".
STAMP="$OVL/.base-stamp"
if [ ! -f "$STAMP" ]; then
    OLDEST="$(find "$OVL" -name '*.page' -exec ls -tr {} + 2>/dev/null | head -1)"
    if [ -n "$OLDEST" ]; then touch -r "$OLDEST" "$STAMP"; else : >"$STAMP"; fi
fi
if find "$NAND" -name '*.page' -newer "$STAMP" -print -quit 2>/dev/null | grep -q .; then
    echo "run-ipod-touch.sh: NOTE -- $(basename "$NAND") has been updated since this" >&2
    echo "  saved state was created, and an overlay SHADOWS what changed, so" >&2
    echo "  anything new in the image (currently: the clipboard daemon) will be" >&2
    echo "  missing. Run once with --fresh to pick it up; that wipes the device" >&2
    echo "  back to clean. To keep the old state, move $OVL aside first." >&2
fi

# --- required for 3.1.3 to boot at all ------------------------------------
# IT_LCD_BRIGHT=255 matters: the panel backlight scales every pixel, and without
# it the screendump/window can look almost black even though the UI is drawn.
export IT_LCD_BRIGHT=255
export IT_DIRECT_IBOOT="$HERE/iBoot.bin"      # 7E18 iBoot, loaded directly
export IT_TVOUT_READY=1                        # TV-out shutdown gates
export IT_TVOUT_VBLANK=1                       # periodic SDO frame interrupt

# --- the code-signing gate, now open on EVERY run -------------------------
# 3.1.3's kernel refuses to exec anything that is not Apple-signed, and it does
# so silently: launchd simply never has a running daemon. That is what killed
# the host<->guest clipboard. /usr/local/bin/it_pbd is baked into nand-grow7g
# and nand-appsync3 (contrib/it-pasteboard), it is our own armv6 binary, and
# without these two args it never runs -- so Cmd+Ctrl+V / `qom-set pasteboard`
# would go on looking like it worked while nothing collected the text.
#
# These are exactly the args --apps and --appsync already set, and they were
# measured to cost nothing: nand-grow7g reaches the lock screen in 30.6 s with
# them and 35.6 s without, at the same lit ratio (0.549), and --small
# (nand-7e18-final) still boots clean at 0.549. Set IT_BOOT_ARGS yourself to
# override, or IT_BOOT_ARGS=" " to bisect against them.
export IT_BOOT_ARGS="${IT_BOOT_ARGS:-amfi_allow_any_signature=1 cs_enforcement_disable=1}"
export IT_BOOT_ARGS_DELAY_MS="${IT_BOOT_ARGS_DELAY_MS:-1500}"
export IT_BOOT_ARGS_REPEAT="${IT_BOOT_ARGS_REPEAT:-200}"
export IT_BOOT_ARGS_INTERVAL_MS="${IT_BOOT_ARGS_INTERVAL_MS:-250}"

# The audio hardware (AMC, CS42L58 codec, I2S0, speaker amp) turns itself on
# with IT_DIRECT_IBOOT, so 3.1.3 gets it and 2.1.1 does not. IT_AUDIO_HW=0
# forces it off if you ever need to bisect against it.

[ "$APPSYNC" = 1 ] && ensure_usbmuxd
# Picked last, immediately before QEMU claims it, to keep the TOCTOU window as
# short as possible -- usbmuxd's startup used to sit inside it. Recorded into
# the session file only now that it is known; scripts drive the guest through
# this port, so it has to be the real one.
QMP_PORT="$(pick_port "$QMP_PORT")"
if [ "$APPSYNC" = 1 ] && [ -f "$SESSION" ]; then
    echo "QMP=127.0.0.1:$QMP_PORT" >>"$SESSION"
fi

# With --appsync we own a usbmuxd child, so QEMU runs as a child too and the
# EXIT trap reaps it. Otherwise exec, as before -- a leaked usbmuxd holding the
# socket makes the NEXT run look like a broken device.
run_qemu() {
    # exec, so that backgrounding this function makes QEMU_PID the QEMU process
    # itself rather than a wrapper subshell. Without it QEMU is a grandchild,
    # and killing the subshell orphans a live emulator.
    exec "$QEMU" \
        -M "iPod-Touch,bootrom=$F/bootrom_240_4,nand=$NAND,nor=$HERE/nor_7E18.bin,nandrw=$OVL$MACHOPTS" \
        -m 128M \
        -display cocoa,show-cursor=on \
        -serial file:"$HERE/serial.log" \
        -qmp tcp:127.0.0.1:"$QMP_PORT",server,nowait \
        ${NETOPTS[@]+"${NETOPTS[@]}"} \
        ${AUDIOOPTS[@]+"${AUDIOOPTS[@]}"} \
        "$@"
}

if [ "$APPSYNC" = 1 ]; then
    trap cleanup EXIT INT TERM
    # QEMU runs in the BACKGROUND and we wait on it. With QEMU in the
    # foreground bash defers the trap until the child exits, so `kill <script>`
    # -- the normal way to stop a backgrounded run or one under a supervisor --
    # did nothing at all and leaked the usbmuxd this script started. A terminal
    # Ctrl-C worked only because the tty signals the whole process group.
    # `wait` is interruptible, so the handler runs immediately.
    run_qemu "$@" &
    QEMU_PID=$!
    wait "$QEMU_PID"
else
    exec "$QEMU" \
        -M "iPod-Touch,bootrom=$F/bootrom_240_4,nand=$NAND,nor=$HERE/nor_7E18.bin,nandrw=$OVL$MACHOPTS" \
        -m 128M \
        -display cocoa,show-cursor=on \
        -serial file:"$HERE/serial.log" \
        -qmp tcp:127.0.0.1:"$QMP_PORT",server,nowait \
        ${NETOPTS[@]+"${NETOPTS[@]}"} \
        ${AUDIOOPTS[@]+"${AUDIOOPTS[@]}"} \
        "$@"
fi
