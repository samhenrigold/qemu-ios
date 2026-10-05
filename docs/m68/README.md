# The original iPhone (M68AP, S5L8900): `-M iPhone-2G`

The iPod touch 1G's machine with the M68's board data (`hw/arm/ipod_touch_1g.c`: `n45_board`, `m68_board`;
`-M iPhone-2G` is a subtype of `iPod-Touch-1G`). iPhone OS 1.0 (1A543a) boots through the S5L8900 bootrom
stubs, iBoot-159, the kernel and the NAND root to SpringBoard. Touch works, lockdownd reports the phone
Activated, and a clean power-off persists across boots. There is no modem yet (another stream): SpringBoard
shows "Repair Needed: iPhone cannot make or receive calls" until a tap dismisses it.

## What runs

- iBoot-159 (no security epoch, so no epoch warning), its
  display (logo, recovery screen), the NAND (WMR C000, four banks), the kernelcache from the HFS root.
  Before the kernel, iBoot talks AT to the baseband on UART1 (`+xgendata`, the radio nvram by
  `+xdrv=9,1,<block>`): with nothing attached each read times out after about 3 s.
- The 1.0 kernel: every driver starts except AppleMRVL868x (see debts) and the baseband stack (no modem).
  The FTL opens, the root mounts read-write, launchd runs.
- lockdownd: FirmwareKit's `conditional-no-record-initializer` strategy (LightTouchMac branch m68):
  "Setting the activation state to Activated", then "Disabling brick mode on the baseband". SpringBoard logs
  "lockdown says the device is: [Activated]". Later boots report "Using the cached activation state" (the data
  ark persisted).
- Touch: the Zephyr1 (`s5l8900.multitouch-z1`): A-Speed bootloader, main firmware stream, reports, frames.
  Taps reach SpringBoard and apps (Settings, Brightness).
- GL: LayerKit composites through the host GL bridge (the 1.x front end matches 1.0's 186 OpenGLES exports),
  or software LayerKit on a device made with `gles_shim` off.
- Power-off: `system_powerdown` (Home, Hold 20 s, slide) ends in `pmu go stdby` and QEMU exits.

Verified 2026-10-04 (branch `m68`): FirmwareKit `m68ap-1A543a` device, home screen, Settings and Brightness
after taps, power-off and three reboots on the same overlay. iPod 1G (FirmwareKit n45ap-3A101a:
`regress.py --device --checks boot`, Settings after a tap) and iPod 2G (`regress.py --checks
boot,fsck,persist`) still pass.

## How to boot

```
firmwarekit create --catalog LightTouchMac/Resources/firmware-catalog.json --id m68ap-1A543a \
  --ipsw iPhone1,1_1.0_1A543a_Restore.ipsw --out DEV --guest-tools GUEST_TOOLS
cp DEV/nor.bin /tmp/nor.bin; chmod u+w /tmp/nor.bin; mkdir -p /tmp/ovl
build/qemu-system-arm -M iPhone-2G,bootrom=$F/ipod1g/bootrom_s5l8900,iboot=DEV/iBoot.bin,nand=DEV/nand,nand-overlay=/tmp/ovl,imei=IMEI,wifi-mac=MAC \
  -drive if=pflash,format=raw,file=/tmp/nor.bin \
  -serial file:/tmp/serial.log -serial MODEM_CHARDEV -display none -audio driver=none -qmp unix:/tmp/qmp.sock,server,nowait
```

IMEI and MAC are `machine.imei` and `machine.wifi-mac` from DEV/device.lock.json. The bootrom is the 1G's
(the same SoC). The second `-serial` is UART1, the baseband port (`serial_hd(1)`; omit it for no modem).
UART3 is the Bluetooth port, silent. iBoot prints its console only with `debug-uarts=3` in the NOR nvram.
SpringBoard is up about 100 s into a boot (guest time); give taps a few seconds after an app opens.

Keys: the 1G's chords plus Cmd+- / Cmd+= for the volume buttons (see debts). Machine properties: everything the 1G has, plus `ring-switch` (bool, on = silent, pad 0x1603; settable at
run time) and `imei` (the unit's, for the modem to report; not used by the machine itself).

## Models: reused, varied, new

Classes as in docs/ipod1g/README.md (R register-level, H high-level, P patch, S stub). Everything not listed is
the 1G's row, unchanged.

| Component | Model | How | Class |
|---|---|---|---|
| Board data | `S5L8900Board` m68_board: PMU/codec bus, codec I2S port and DMA request, buttons, touch, NAND banks, power-off timing | data, not a fork | — |
| DRAM | + an alias at +0x80000000 (both machines): iBoot-159 DMAs NAND pages to its heap's uncached VA (0x98…) | SoC decode | R |
| GPIO | `fsel-offset=0x320` (both machines): the 1.x kernel drives output pads through it (the 2G's is 0x1e0) | property | R |
| PMU, WM8758 | on I2C0 (the 1G's are on I2C1) | board data | H |
| Codec data | I2S0 at 0x3CA00000, dmac0 request 0, ready GPIO-IC 0x86, host output on (no piezo on the M68) | board data | R |
| I2S1 (baseband audio), I2S at 0x3D400000 | RAM windows | — | S |
| Buttons | menu 0x1600, volume up/down 0x1601/0x1602 (active low: the GPIO block's `rest-high-*` keeps them high at rest), Hold 0x1605, ring switch 0x1603; GPIO-IC 0x28-0x2d, interrupting by the polarity and type the driver programs; a press lasts at least 150 ms of guest time | board data / SYSIC | R |
| Zephyr1 | `s5l8900.multitouch-z1`, subtype of the shared `ipodtouch.multitouch` (host input, frames and ATN shared; the Zephyr2 unchanged), chip select on GPIO 0x0705, ATN GPIO-IC 0xa3 | new | H (firmware) / R (wire) |
| NAND | FMC `banks=4` (ID reads answer only populated chip enables); ADM firmware-14's transfer block (found by its data-section pointers, 0x824 into data2) and page list at +0x444 | property / variant | R / H |
| UART1, UART3 | `cts` on: iBoot-159 waits for CTS before each byte it sends; with nothing attached the far end reads ready | property | R |
| USB wrangler quirk | remove()'s vtable slot read from the matched instruction (0x94 in 1.0, 0x54 in 1.1) | P, as the 1G's | P |
| Baseband, Bluetooth, camera, ALS | not modelled (UART endpoints only; I2C NACKs) | — | — |

The Zephyr1 wire protocol (openiBoot's `multitouch-z1.c`, and 1.0's AppleMultitouchSPI where they differ):
`C2` data packets (A-Speed) and a blank `C2 00 00 00` before the main firmware stream, `05 00 00 06` verify
(`D0 00` + 16-bit sum), `C4` execute, `D0` interface version, `8F` report info, `82` report, `46` frame
length (`AA len len ck ck` for interface versions up to 0x10), `47` frame data (`AA` + frame + sum). The model
reports interface version 1 and the Zephyr2 model's sensor profile. `MT_TRACE=2` logs every transaction.

## Debts

1. **Wi-Fi: no calibration.** iBoot copies the Wi-Fi tx-calibration into the DT (`arm-io/sdio`
   `tx-calibration`) from the baseband's nvram ("WIFI Calibration Data" entry, read with `+xdrv=9,1,<block>`).
   With no modem it stays zeros, and AppleMRVL868x refuses it ("Invalid calibration data in device tree": it
   accepts any first 128 bytes that are neither all 0x00 nor all 0xFF). Settings shows "No Wi-Fi". The modem
   model serving that nvram entry fixes it without touching the guest.
2. **No modem** (another stream): "Repair Needed" alert, no carrier, no IMEI in the DT (so lockdownd's view
   of the UDID lacks it; FirmwareKit's identity hashes the IMEI it records).
3. **1.0's slow power-off sheet**: the gesture holds Hold 20 s (the sheet came up 13 s into a hold in one
   run). A run that releases too early locks the phone instead (`pmu go hib`).
4. **Touch calibration.** The frames use the Zephyr2 model's sensor profile. Taps land on their targets at the
   few points checked (Dismiss, icons, table rows), but no calibration fit was done as for the K48.
5. The 1G's debts apply as they are (wake from sleep, AES convention, CLCD, timers 0-3).

## Files

`hw/arm/ipod_touch_1g.c` (board table, machine types), `include/hw/arm/ipod_touch_1g.h`,
`hw/arm/s5l8900_multitouch_z1.c` and its header, `hw/arm/s5l8900_{fmc,adm}.c`, `hw/arm/ipod_touch_gpio.c`,
`hw/arm/ipod_touch_spi.c`, `hw/char/exynos4210_uart.c`, `contrib/guest-package/mkpkg.py` (the 1.x family
covers 1.0 and m68ap). The device is made by FirmwareKit's m68 recipe (`N45Recipe.swift`, LightTouchMac).
