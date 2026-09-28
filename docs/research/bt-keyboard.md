> Status: research, superseded by `../ipad1/usb-keyboard.md` (Bluetooth dropped 2026-09-26).

# iPad 1 (7B500): emulated Bluetooth HID keyboard, scoping

**Status: dropped (2026-09-26).** The target keyboard is USB via the CCK host path; see ../ipad1/usb-keyboard.md.

This is research only; nothing here has been built. The goal is a hardware keyboard that uses only stock
iOS code: BlueTool, BTServer, and the kernel UART/DMA drivers, all unmodified. The emulator plays both
the BCM4329's Bluetooth side and a remote keyboard that is already paired. The stopgap until then is
`contrib/ipad1-hidbridge` (keyboard-and-network.md §1). Its host key map and 8-byte boot reports
carry over unchanged, because a BT HID keyboard sends the same reports on its interrupt channel.

Sources: `7B500/dec/DeviceTree.txt`, `mnt-rootfs/private/etc/bluetool/*`, `usr/sbin/BTServer`
(MobileBluetooth-75.26) strings, `hw/arm/ipod_touch_bt.c`, `hw/arm/ipad1.c`, `hw/arm/s5l8930_cdma.c`.

## Is it the iPod's path? Partly

| | iPod touch 2G (covered) | iPad 1 |
|---|---|---|
| Controller | BCM4325, `bluetooth,n72` | BCM4329B1, `bluetooth,n88` |
| UART | uart1, PIO | **uart3** (0x82800000, IRQ 0x19), **CDMA**: `dma-channels` 0x0c (TX, FIFO 0x82800020) and 0x0d (RX, FIFO 0x82800024) |
| Speed / encoding | H4 | `transport-speed` 3,000,000, `transport-encoding` 3 (H4) |
| Reset / wake | — | `bt_reset` = TCA6408 pin 0, `bt_wake` = GPIO 0x7/0x101 |
| Bring-up | BTServer HCI_Reset | BlueTool `iPad1,1.boot.script` (below), then BTServer |

The HCI byte protocol is the same, so `ipod_touch_bt.c`'s H4 parser, reply timer and command-complete
table can be reused as they are. The transport underneath is new:

1. `ipad1.c` creates only UART0. UART3 needs `exynos4210_uart_create` at `S5L8930_UART_BASE(3)`, with the
   BT chardev attached.
2. **DMA.** The A4 UART driver uses CDMA peripheral channels for BT. `s5l8930_cdma.c` runs a whole chain
   inside the `go` write (memory/AES only). BT needs a paced device-FIFO mode: TX drains to the UART,
   and RX completes as bytes arrive, including the Rx-timeout / partial-descriptor behaviour the
   iPod comment warns about. **This is the largest unknown.** First check in the kext whether
   AppleS5L8930XSerial falls back to PIO when the DMA channels are absent from the DT. If it does,
   drop `dma-channels` from our DT and this item goes away.
3. BlueTool's script needs these vendor commands answered: `reset pulse`/`wake` (GPIO/expander, no HCI);
   `bcm -B` (baud, 0xfc18); `bcm -m` (reading the module id; the script branches on it); `bcm -w`
   (patchram download, 0xfc2e then a stream of 0xfc4c writes then 0xfc4e launch); `bcm -a` (BD_ADDR,
   0xfc01); `bcm -f`, `-s` (sleep mode 0xfc27), `-p` (PCM route 0xfc1c). Each gets a status-only
   Command Complete. The opcodes are the usual Broadcom ones; confirm them with `IT_BT_TRACE=1`.

## What a keyboard needs beyond command-complete

`ipod_touch_bt.c` today has "no ACL/SCO data path and no link control". A paired keyboard needs:

1. **Pairing, pre-seeded offline.** BTServer stores paired devices and link keys in the
   `com.apple.MobileBluetooth` plists under `/var/mobile/Library/Preferences`: `RoleHID`,
   `HIDDescriptor`, `HIDVendorID`, `HIDProductID`, `HIDVersion`, plus the link key. We write these
   into the NAND image with a fixed fake BD_ADDR and a fixed link key, so the guest never runs
   inquiry or pairing UI. The exact plist schema still has to be read out of BTServer (the
   `PincodeStorage.cpp` / device-record code); that is about half a day of RE. Doing interactive
   pairing instead (inquiry result, name, SSP/pincode events) is possible, but it's more events and
   more UI to get through.
2. **Link control.** For a reconnect started by the keyboard: `Connection Request` event → guest
   `Accept Connection Request` → `Connection Complete` (a handle). Then
   `Link Key Request` → guest `Link Key Request Reply` (the key from step 1) → `Auth Complete` →
   `Set Connection Encryption` → `Encryption Change`. Also answer `Read Remote Features`,
   `Remote Name Request` and role/mode commands (`Write Link Policy`, `Sniff Mode`) as the trace
   shows them, plus `Number Of Completed Packets` for ACL flow control.
3. **L2CAP** on the ACL handle: signalling channel (CID 1) with Connection Req/Rsp, Configure Req/Rsp
   both ways, and Disconnect. The keyboard opens (or accepts) PSM 0x11 (HID control) and PSM 0x13
   (HID interrupt). BTServer may also run **SDP** (PSM 1) against the keyboard ("SdpClient",
   `getSdpAttributeForService`). The pre-seeded HID attributes may let it skip SDP. If they don't,
   serve a canned SDP record with the HID descriptor.
4. **HID.** On the interrupt channel, input reports go out as `A1 <8-byte boot report>`. The control
   channel needs `SET_PROTOCOL`/`GET_REPORT`/`SET_REPORT` handshakes (LED output reports), answered
   `00` (successful) or with a canned report.
5. BTServer then goes through `InputIOHIDClass` → `IOHIDUserDeviceCreate`, which is the same kernel
   path hidbridge already exercises on the real unit.

## Cost

| Piece | Estimate |
|---|---|
| UART3 + BT chardev on ipad1, BlueTool vendor commands | 1 day |
| CDMA device-FIFO pacing (skip it if the PIO fallback exists) | 0-4 days, highest risk |
| Pre-seeded pairing plist (RE the schema, imgtools option) | 1 day |
| Link control + encryption event sequence | 1-2 days |
| L2CAP signalling + HID control/interrupt (+ canned SDP) | 2-3 days |
| Host input → interrupt-channel reports (reuse `MAC_TO_HID`) | 0.5 day |
| **Total** | **~1.5-2.5 weeks**, versus 1-2 days for hidbridge |

Debugging is guest-blind: BTServer logs little. Enabling `IT_BT_TRACE` and adding an HCI/L2CAP packet
log on the QEMU side from day one makes most of the event-ordering problems visible.

It also buys the iPod touch 2G the same capability, since that stack is 2.x BTServer with the same
HCI model. That port is untested: 2.x UIKit has no hardware-keyboard consumer (see it_kbd_agent),
so for the iPod it would only help a 3.x+ guest.
