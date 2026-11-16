#!/bin/bash
# GATE: models
# Every machine starts with the -machine options LightTouchMac passes it, and stops at -S.
#
# LightTouchMac builds each board's -machine string (HostRuntime BootRecipe and PreparedDeviceBoot) from its prepared
# base's options, and refuses to pass one its machine doesn't declare (HostRuntime MachineOptions, listed from this
# tree by its scripts/machine-options); here each machine gets that whole set, with values of the app's form, and must
# get through option parsing and board init to the QMP prompt: a property one board lacks (wifi-country on n88 before
# it had one) or a value it refuses stops QEMU before the guest runs, which is how a device dies before its screen
# lights. QEMU= selects the binary (default build/qemu-system-arm).
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
QEMU="${QEMU:-$ROOT/build/qemu-system-arm}"
T="$(mktemp -d /tmp/machine-options.XXXXXX)"
trap 'rm -rf "$T"' EXIT
# A K48KBOOT bundle with an empty image (the trailer only), a NOR and its private copy, empty NAND and package
# directories, a bootrom. nand= and nand-overlay= are left out where the machine wants an image store's geometry.json.
printf 'K48KBOOT\0\0\0\100\0\0\0\100\0\0\0\100\0\0\0\0' > "$T/kboot"
head -c 1048576 /dev/zero > "$T/nor"
cp "$T/nor" "$T/nor-rw"
head -c 65536 /dev/zero > "$T/rom"
mkdir "$T/nand" "$T/overlay" "$T/package"
fail=0

# NAME MACHINE-OPTIONS [ARGS...]: start it at -S and quit over QMP; anything QEMU prints to stderr is a failure.
start() {
    local name=$1 options=$2 out; shift 2
    out="$(printf '%s\n' '{"execute":"qmp_capabilities"}' '{"execute":"quit"}' |
        "$QEMU" -M "$name,$options" "$@" -S -display none -nodefaults -audio driver=none -serial none -qmp stdio \
        2>&1 >/dev/null)"
    if [ $? -eq 0 ] && [ -z "$out" ]; then
        echo "PASS $name"
    else
        echo "FAIL $name: ${out:-exit status}"
        fail=1
    fi
}

COMMON="usb-tcp-addr=127.0.0.1:9,guest-package=$T/package,rtc-epoch=1300000000"
KBOOT="kboot=$T/kboot,nor-rw=$T/nor-rw,die-id=0x123:0x456,$COMMON,wifi-country=DE"
start n18 "$KBOOT,panel=320x480"
start n88 "$KBOOT,panel=320x480,baseband=on,imei=013977000272744"
start ipad1 "$KBOOT,panel=1024x768,wifi-mac=02:11:22:33:44:66"
start iPod-Touch-4G "$KBOOT,panel=640x960"
start iPhone-4 "$KBOOT,panel=640x960,baseband=on,imei=013977000272744"
start iPod-Touch "boot-args=,direct-iboot=,direct-llb=,bootrom=$T/rom,nand=$T/nand,nor=$T/nor,nor-rw=$T/nor-rw,nandrw=$T/overlay,$COMMON,osk=on,wifi=on,gid-blobs=$T/rom,aes-uid=hardware,wifi-mac=02:11:22:33:44:66,bt-mac=02:11:22:33:44:67,ecid=0x234,panel=320x480"
ONEG="bootrom=$T/rom,iboot=$T/rom,nand=$T/nand,nand-overlay=$T/overlay,$COMMON,wifi-mac=02:11:22:33:44:66,panel=320x480"
start iPod-Touch-1G "$ONEG" -drive "if=pflash,format=raw,file=$T/nor"
start iPhone-2G "$ONEG,imei=013977000272744" -drive "if=pflash,format=raw,file=$T/nor"

# wifi-country reaches the Wi-Fi card: a country that isn't two letters is the card's to refuse.
for name in n18 n88 ipad1 iPod-Touch-4G iPhone-4; do
    out="$("$QEMU" -M "$name,kboot=$T/kboot,wifi-country=USA" -S -display none -nodefaults -audio driver=none \
        -serial none -monitor none 2>&1 </dev/null)"
    case "$out" in
        *"country must be two letters"*) echo "PASS $name: the card refuses wifi-country=USA" ;;
        *) echo "FAIL $name: wifi-country=USA did not reach the card: $out"; fail=1 ;;
    esac
done
exit $fail
