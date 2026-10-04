#!/bin/sh
# Real-iPad register oracle: boot a patched 7B500 iBEC that adds `md <addr> [n]` and `mw <addr> <val>`
# console commands, then read registers over USB. Non-destructive: nothing touches NAND or NOR.
#
#   session1.sh build   patch + wrap iBSS/iBEC into $OUT (needs imgtools/ipad1_fw.py output in $DEC)
#   session1.sh pwn     limera1n the iPad (must already be in DFU mode)
#   session1.sh boot    load pwned iBSS, then the md/mw iBEC
#   session1.sh probe   run probes.txt through the USB console, appending to $OUT/session1.log
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
LIK=${LIK:-~/Downloads/Legacy-iOS-Kit_complete_v25.09.01/bin/macos}
DEC=${DEC:-~/Developer/qemu-ios-files/ipad1/7B500/dec}
IPSW=${IPSW:-~/Downloads/ipad1-ios32-feasibility/iPad1,1_3.2.2_7B500_Restore.ipsw}
OUT=${OUT:-~/Developer/qemu-ios-files/ipad1/payload}
IREC=$LIK/arm64/irecovery
mkdir -p "$OUT" && cd "$OUT"

case "$1" in
build)
    unzip -oqj "$IPSW" 'Firmware/dfu/*'
    $LIK/arm64/iBoot32Patcher "$DEC/iBSS.bin" iBSS.patched --rsa >/dev/null || test -s iBSS.patched  # exits nonzero even on success
    $LIK/arm64/xpwntool iBSS.patched pwnediBSS.dfu -t iBSS.k48ap.RELEASE.dfu
    clang -target thumbv7-none-eabi -c "$HERE/mdmw.s" -o mdmw.o
    python3 "$HERE/build_ibec.py" "$DEC/iBEC.bin" iBEC.mdmw.bin
    $LIK/arm64/xpwntool iBEC.mdmw.bin iBEC-mdmw.dfu -t iBEC.k48ap.RELEASE.dfu ;;
pwn)
    $LIK/ipwnder -p || $LIK/primepwn ;;
boot)
    $IREC -f pwnediBSS.dfu; sleep 3
    $IREC -f iBEC-mdmw.dfu; $IREC -c go; sleep 3
    $IREC -q ;;
probe)
    sed 's/#.*//;/^ *$/d' "$HERE/probes.txt" | while read -r cmd; do echo "$cmd"; sleep 0.3; done \
        | $IREC -s | tee -a session1.log ;;
*)
    sed -n '2,9p' "$0"; exit 2 ;;
esac
