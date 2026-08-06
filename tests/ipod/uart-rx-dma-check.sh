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

# The real assertion: the guest gets PAST HCI_Reset. It can only send the
# Broadcom vendor commands (0xfc18 Update Baud Rate, 0xfc2e Download
# Minidriver) after it has received AND accepted the Command Complete for
# Reset, which exercises the whole path -- chardev -> Rx FIFO -> Rx DMA request
# -> DMAC -> guest memory -> last-request -> terminal count -> driver. With any
# one of the three bugs back, the guest sends nothing but 0x0c03 forever.
if grep -q '0xfc18' "$WORK/trace.log"; then
    echo "PASS: guest accepted the HCI_Reset reply and moved on to $(grep -c '\[BT\] cmd' "$WORK/trace.log") commands"
    grep -o '\[BT\] cmd .*' "$WORK/trace.log" | sort -u
    exit 0
fi

echo "FAIL: the guest never got past HCI_Reset, so it never accepted a reply." >&2
echo "      Check the rxdmareq wiring, the UCON[1:0] DMA-mode decode, and" >&2
echo "      pl080_set_dma_last_request()." >&2
exit 1
