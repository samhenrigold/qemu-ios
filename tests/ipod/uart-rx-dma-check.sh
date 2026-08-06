#!/bin/bash
#
# Does a byte handed to a UART chardev actually reach guest memory?
#
# It did not, for the whole life of this tree, and nothing noticed because
# nothing was ever connected to a UART's input. Two bugs stacked: the Rx DMA
# REQUEST line was never driven (hw/arm/ipod_touch_2g.c), and UCON[1:0]==11b --
# what the S5L8720 uses to select DMA -- was decoded as "not DMA"
# (hw/char/exynos4210_uart.c). The guest's Bluetooth driver arms a 2048-byte
# peripheral->memory channel on UART1's URXH and never reads the register
# itself, so replies sat in the Rx FIFO forever.
#
# The check: boot with the Bluetooth HCI model on UART1, and assert the guest's
# DMA channel residue moved. 0x800 -> 0x7f9 is exactly the seven bytes of one
# HCI Command Complete, pulled out of the FIFO by the DMAC and written to guest
# memory. If either bug comes back this reads 0x88000800 forever.
#
#     tests/ipod/uart-rx-dma-check.sh [nand-image]
#
# NOTE: this asserts the TRANSPORT works, not that the guest's Bluetooth stack
# completes bring-up -- it does not yet. See the launch-animation notes.
set -u

QEMU="${QEMU:-$(cd "$(dirname "$0")/../.." && pwd)/build/qemu-system-arm}"
F="$HOME/Developer/qemu-ios-files"
NAND="${1:-$F/nand-testapps}"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"; [ -n "${PID:-}" ] && kill "$PID" 2>/dev/null' EXIT

IT_LCD_BRIGHT=255 IT_DIRECT_IBOOT="$F/ios3/iBoot.bin" IT_WDT_NORESET=1 \
IT_TVOUT_READY=1 IT_TVOUT_VBLANK=1 \
IT_BOOT_ARGS="amfi_allow_any_signature=1 cs_enforcement_disable=1" \
IT_BOOT_ARGS_DELAY_MS=1500 IT_BOOT_ARGS_REPEAT=200 IT_BOOT_ARGS_INTERVAL_MS=250 \
IT_BT_TRACE=1 IT_DMAC_TRACE=1 \
"$QEMU" -M "iPod-Touch,bootrom=$F/bootrom_240_4,nand=$NAND,nor=$F/ios3/nor_7E18.bin,nandrw=$WORK/ovl" \
    -m 128M -display none -serial file:"$WORK/serial.log" \
    >"$WORK/trace.log" 2>&1 &
PID=$!
sleep 90
kill "$PID" 2>/dev/null; PID=""

cmds=$(grep -c '\[BT\] cmd' "$WORK/trace.log")
echo "HCI commands the guest sent: $cmds"
if [ "$cmds" -lt 1 ]; then
    echo "FAIL: no HCI command reached the controller. Either UART1 Tx is" >&2
    echo "      broken, or the model is switched off (IT_BT=0)." >&2
    exit 1
fi

# Any residue below 0x800 on the UART1 Rx channel means the DMAC moved bytes.
if grep -qE 'R 14c Control        8800(07|06|05)' "$WORK/trace.log"; then
    echo "PASS: guest DMA drained the HCI reply out of the Rx FIFO"
    grep -m1 -E 'R 14c Control        8800(07|06|05)' "$WORK/trace.log"
    exit 0
fi

echo "FAIL: Rx DMA residue never moved -- bytes handed to the chardev did not" >&2
echo "      reach guest memory. Check the rxdmareq wiring and the UCON[1:0]" >&2
echo "      DMA-mode decode." >&2
grep -m3 'R 14c Control' "$WORK/trace.log" >&2
exit 1
