# The original iPhone (M68AP, S5L8900): `-M iPhone-2G`

The iPod touch 1G's machine with the M68's board data (`hw/arm/ipod_touch_1g.c`: `n45_board`, `m68_board`;
`-M iPhone-2G` is a subtype of `iPod-Touch-1G`). iPhone OS 1.0 (1A543a) boots through the S5L8900 bootrom
stubs, iBoot-159, the kernel and the NAND root to SpringBoard. Touch works, lockdownd reports the phone
Activated, and a clean power-off persists across boots. The shared baseband model (`ios-baseband`) sits on
UART1: carrier "LightTouch" with full bars and EDGE, Wi-Fi up, SMS in, calls in and out.

## What runs

- iBoot-159 (no security epoch, so no epoch warning), its
  display (logo, recovery screen), the NAND (WMR C000, four banks), the kernelcache from the HFS root.
  Before the kernel, iBoot talks AT to the baseband on UART1 (`+xgendata`, the radio nvram by
  `+xdrv=9,1,<block>`): the modem answers both ("Read 1536 bytes from nvram"), so iBoot puts the Wi-Fi
  calibration into the DT and no radio timeout delays the boot.
- The 1.0 kernel: every driver starts, AppleMRVL868x included (it accepts the modem's calibration).
  The FTL opens, the root mounts read-write, launchd runs.
- lockdownd: FirmwareKit's `conditional-no-record-initializer` strategy (LightTouchMac branch m68):
  "Setting the activation state to Activated", then "Disabling brick mode on the baseband". SpringBoard logs
  "lockdown says the device is: [Activated]". Later boots report "Using the cached activation state" (the data
  ark persisted). A data-only route does not work on 1.0 (tried, as Lakr233's qemu-ios-4 does for iOS 4: a
  seeded `data_ark.plist` with `com.apple.mobile.lockdown_cache-ActivationState` Activated or
  FactoryActivated, `-BrickState` false, vanilla lockdownd). lockdownd uses the cached state, but any change in
  the baseband's ICCID (empty at first, then the SIM's) triggers an activation check, and with no Apple-signed
  `activation_record.plist` that check sets Unactivated and brick mode: the branch the strategy rewrites.
- Touch: the Zephyr1 (`s5l8900.multitouch-z1`): A-Speed bootloader, main firmware stream, reports, frames.
  Taps reach SpringBoard and apps (Settings, Brightness).
- GL: LayerKit composites through the host GL bridge (the 1.x front end matches 1.0's 186 OpenGLES exports),
  or software LayerKit on a device made with `gles_shim` off.
- Modem: CommCenter brings up H5, the 27.010 mux (DLCI 0-7) and its init sequence; registration on the
  001/01 test network, "LightTouch", five bars, EDGE; no "Repair Needed" (only an "iPhone is activated"
  alert on the first boot). Incoming SMS (alert and Messages thread), incoming calls (ring, Answer, remote
  hang-up), outgoing calls from the keypad (`last-dialed`, `remote-answer` with `+COLP` so the in-call screen
  shows the number, End Call from the UI).
- Cellular data (EDGE) with `-netdev user,id=cell0` (the modem looks that name up; LightTouchMac adds it).
  CommCenter defines and activates the context (`+cgdcont`, `+cgact`, `+xdns`, `+cgpaddr`), then
  `+cgdata="M-RAW_IP",1` turns DLCI 6 into raw IPv4, which the modem bridges to slirp. With `wifi=off`,
  Safari's Apple bookmark goes over it: DNS, TCP and `GET /iphone/start/` reach www.apple.com, which
  redirects to HTTPS; 1.0's Safari can't negotiate today's TLS ("could not establish a secure connection").
- Power-off: `system_powerdown` (Home, Hold 20 s, slide) ends in `pmu go stdby` and QEMU exits.

Modem verified 2026-10-05 on 1A543a with every item above, by QMP and screenshots.
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
  -serial file:/tmp/serial.log -display none -audio driver=none -qmp unix:/tmp/qmp.sock,server,nowait
```

IMEI and MAC are `machine.imei` and `machine.wifi-mac` from DEV/device.lock.json. The bootrom is the 1G's
(the same SoC). UART1 is the modem's; `baseband=off` takes the model off and leaves UART1 a plain
`serial_hd(1)` (with nothing there iBoot's AT reads time out after about 3 s each). UART3 is the Bluetooth port, silent. iBoot prints its console only with `debug-uarts=3` in the NOR nvram.
SpringBoard is up about 100 s into a boot (guest time); give taps a few seconds after an app opens.

Keys: the 1G's chords plus Cmd+- / Cmd+= for the volume buttons (see debts). Machine properties: everything the 1G has, plus `ring-switch` (bool, on = silent, pad 0x1603; settable at
run time), `imei` (the unit's, reported by the modem's `+CGSN`), `baseband` (default on).

Modem controls, aliased from the `baseband-modem` child onto `/machine` (qom-get/qom-set):
`carrier`, `mcc-mnc`, `signal-dbm`, `registered`, `sim-present`, `imsi`, `iccid`, `voicemail`,
`answer-delay-ms`; actions (write any string) `remote-answer`, `remote-hangup`, `incoming-call` (a number),
`incoming-sms` (`"<number>|<text>"`); read-only `call-state`, `last-dialed`, `last-mo-sms`. The board feeds
the modem `battery-percent` from the PMU's battery ADC every 10 s. The core is shared with the other iPhones
(`hw/misc/ios_baseband_core.c`, `tests/unit/test-ios-baseband.c`); 1.0 needed: CONFIG with no config field
(the window comes from our CONFIG RESP), the `+xtransportmode` OK sent over H5, the data CRC either byte
order, flags on every mux frame inside H5, and the radio nvram (`+xdrv=9,1,<block>`, 512-byte blocks:
entry type 1 = Wi-Fi calibration, 1024 bytes, a generated non-constant pattern).

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
| UART1 receive FIFO | 16 deep on the M68 (`rx-size`): the 4-bit UFSTAT count wraps at 256 | property | R |
| Baseband | `ios-baseband` on UART1 (H5, 27.010 mux, AT engine, radio nvram) | shared model | H |
| Bluetooth, camera, ALS | not modelled (UART3 endpoint only; I2C NACKs) | — | — |

The Zephyr1 wire protocol (openiBoot's `multitouch-z1.c`, and 1.0's AppleMultitouchSPI where they differ):
`C2` data packets (A-Speed) and a blank `C2 00 00 00` before the main firmware stream, `05 00 00 06` verify
(`D0 00` + 16-bit sum), `C4` execute, `D0` interface version, `8F` report info, `82` report, `46` frame
length (`AA len len ck ck` for interface versions up to 0x10), `47` frame data (`AA` + frame + sum). The model
reports interface version 1 and the Zephyr2 model's sensor, with its own frame calibration: taps land
within 0.1 px of their aim over the whole panel (fitted from the points 1.0's GraphicsServices reports,
`GSEventGetLocationInWindow`, read through the gdbstub). `MT_TRACE=2` logs every transaction.

## Debts

1. **Modem gaps.** Unanswered commands get OK: `+crsm`, `+cnum`, `+xcfc`,
   `+xctms`, `+xdtmf`, `+cclk`, `+xlog`. No audio. Messages formats the sender oddly
   ("+55 51 234").
2. **Taps lag.** The guest UI takes tens of seconds to open an app on a cold boot; scripted taps must wait for
   screenshots, not fixed delays. A tap sent before a view is up is lost, which looks like dropped keypad
   digits. Once the keypad is up, taps 0.25 s apart all register (8 of 8).
3. **1.0's slow power-off sheet**: the gesture holds Hold 20 s (the sheet came up 13 s into a hold in one
   run). A run that releases too early locks the phone instead (`pmu go hib`).
4. **lldb killed mid-command** (guestdev): twice a boot stopped taking taps after an lldb attached to the
   gdbstub was killed by `timeout` (the CPU idles taking serial interrupts, no syscalls). Not root-caused; a
   clean detach is fine.
5. The 1G's debts apply as they are (wake from sleep, AES convention, CLCD, timers 0-3).

## Notes for guest software

- Accelerometer: lying flat, 1.0's UIKit reads z = -0.43 g (Tilt), as a real iPhone on 1.0 does. The
  LIS302DL model reports the datasheet's 18 mg per count at ±2 g (72 mg with CTRL_REG1's FS bit), so 1 g is
  55.6 counts. 1.0 and 1.1.4's AppleLIS302DL turn a count into g as `count << 9` (1/128 g per count), 3.1.3's
  as 1187/65536 (18 mg), so the same part reads 0.43 g on 1.0 and 1.00 g on 3.x.
- 1.0's SpringBoard labels an icon with the `.app` directory name and ignores `CFBundleName` (guestdev:
  Hello2.app with CFBundleName "Hello 2" shows "Hello2").
- Web proxy (LightTouchMac's, 10.0.2.100:3128 on the wifi0 guestfwd). 1.x reads its SystemConfiguration
  preferences from root's home (~/Library/Preferences/SystemConfiguration), where FirmwareKit now writes
  the known network and the PAC'd AirPort service. Wi-Fi then joins on demand and Safari follows the PAC:
  a plain GET through the proxy, and for HTTPS a CONNECT and a TLS 1.0 ClientHello (AES128-SHA, RC4,
  3DES; SSLv3 retry). For the proxy's HTTPS, 1.0 has to trust its CA. 1.0's SecTrust consults only the
  system anchors, `/System/Library/Frameworks/Security.framework/TrustStore.sqlite3` (table tsettings;
  the user store is never asked). A row there makes it an anchor:
  - sha1 of the certificate;
  - subj = the subject Name's content with PrintableString values uppercased (UTF8String and T61String
    kept as issued);
  - tset = the empty-array plist the stock rows carry;
  - data = the certificate.
  With a CA and leaf shaped like WebProxyCA's and that row, Safari opened an HTTPS page through a TLS 1.0
  relay (lock icon; SecTrustEvaluate's verdict 4). Without it, the verdict is deny and Safari says it
  "could not establish a secure connection". No "continue" prompt in 1.0.
- Sideloading unofficial apps: [sideload.md](sideload.md). Debugging the guest (gdbstub, lldb, `xnu.py`):
  [../guest-debug.md](../guest-debug.md). 1.0 1A543a, 2026-10-05: kernel and userland symbols PASS (`xnu-procs`;
  SpringBoard `mach_msg` stop with libSystem/CoreFoundation frames); no debugserver exists for 1.0.

## Files

`hw/arm/ipod_touch_1g.c` (board table, machine types), `include/hw/arm/ipod_touch_1g.h`,
`hw/arm/s5l8900_multitouch_z1.c` and its header, `hw/arm/s5l8900_{fmc,adm}.c`, `hw/arm/ipod_touch_gpio.c`,
`hw/arm/ipod_touch_spi.c`, `hw/char/exynos4210_uart.c`, `contrib/guest-package/mkpkg.py` (the 1.x family
covers 1.0 and m68ap). The device is made by FirmwareKit's m68 recipe (`N45Recipe.swift`, LightTouchMac).
