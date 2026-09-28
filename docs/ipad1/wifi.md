# iPad 1 (7B500) Wi-Fi: BCM4329

**Status: default network (2026-09-27, Sam).** `-machine ipad1` has `wifi=on` unless `wifi=off` is
given, and it creates `-netdev type=user,id=wifi0` itself when no `wifi0` netdev exists. The stock kernel
and AppleBCMWLAN join the open BSS "qemu-ios" at about 36 s into boot with no seed, so nothing is needed
on the host side.

Soak on a golden clone: boot, Safari page, Home, sleep/wake, rotate both ways, 10 min idle then Safari
reload, reboot (system_reset), rejoin, Safari again. All passed after one fix:

- The card model had no reset. A rebooted guest found a card claiming its firmware was up, never finished
  the download, and panicked after AppleBCMWLAN's 60 s watchdog. `ipodtouch.sdio` and `s5l8930.sdio`
  now reset. This also applies to the iPod.

## Status (2026-09-27): works, stock stack, no guest changes

A golden-pristine clone (`wifi=on` was explicit at the time). The serial log:

```
IOSDIOIoCardDevice::parseFn0CIS(): Device manufacturer ID 0x2d0, Product ID 0x4329   ProductInfo0 "s=B1"  ProductInfo1 "P=K48 m=u80"
AppleBCMWLANChipManager::withDriver(): BCM4329 revision B1
AppleBCMWLAN::initFirmware(): successful initialization
AppleBCMWLAN: Ethernet address <unit Wi-Fi MAC>
AirPort: Enabled AppleBCMWLAN (link 1, sys 0, user 1)
AppleBCMWLAN Joined BSS: ... BSSID = 02:00:5e:10:00:01, rssi = -45, channel = 6, ssid[ 8] = "qemu-ios"
AppleBCMWLANNetManager::receivedIPv4Address(): Received IP Address
```

What the screen showed:

- The status bar has the Wi-Fi icon, and Settings lists Wi-Fi as "qemu-ios"
  (screens/wifi-settings.png).
- Safari loaded a page served on the host at `http://10.0.2.2:8765/` over the
  emulated link (screens/wifi-safari.png).
- A 90 s screen-off, then wake and reload, fetched the page again. There were no
  deepsleep errors and no timeouts.

How it's built (implementation notes):

- `hw/arm/s5l8930_sdio.c` handles IOP ring 3 (ops 2 to 7) and the SDHCI
  card-interrupt registers, as §1 describes.
- The card is the iPod's `ipodtouch.sdio` dongle model with a `BCMSDIOChip`
  identity: MANFID 0x2d0/0x4329, the VERS_1 strings, ChipID 0x00034329 (rev 3 =
  B1) and the unit's MAC.
- The only protocol difference that mattered was the SDIO device core address.
  It's 0x18011000 on the 4329 and 0x18002000 on the 4325.
- The dongle model detects CDC 16 / BDC 4 on 2.60 by itself.

Former cosmetics, all fixed:

- `parseFunctionExtension(): @2 - Error! No space for body of 0x02`. AppleBCMWLAN-2.60 reads every
  FUNCE body as {type, len, data} records, so the 4329's CIS leaves out function 0's common FUNCE
  (`BCMSDIOChip.no_common_funce`).
- `BCMWLAN Firmware Version:` was empty. It now answers the `ver` iovar:
  `wl0: Jul 21 2010 21:58:50 version 4.218.175.43`.
- The Wi-Fi Networks pane used to draw black rectangles. It now renders with "✓ qemu-ios"
  (screens/wifi-networks.png), after the CoreAnimation texture fixes.

The rest of this document is the feasibility study that preceded the work.

The question is whether the stock 3.2.2 Wi-Fi stack can run unpatched against an
emulated BCM4329, and at what cost. It is research only; nothing is implemented yet.

**Short answer: feasible, about 2 to 3 weeks.** The dongle protocol has already been
solved in this tree. The iPod machine's `hw/arm/ipod_touch_sdio.c` is a BCM4325
dongle model that an unmodified Broadcom driver scans, joins and runs DHCP over,
far enough to load a page in Safari (docs/networking.md). The iPad needs three
things:

- a new transport, because SDIO goes through the IOP here rather than a
  host-controller block the AP drives itself
- the 4329's chip identity
- whatever the newer AppleBCMWLAN-2.60 driver does differently from the iPod's
  `AppleBCM4325`

The earlier "3-6 weeks, don't build it" estimate in ../research/keyboard-and-network.md §3
assumed no reference implementation. There is one, a few files over.

## 1. How 7B500 reaches the chip

```
AppleBCMWLAN (Broadcom 802.11 Driver, AppleBCMWLAN-2.60)
  AppleBCMWLAN4329ChipManager, AppleBCMWLANK48PlatformManager, AppleBCMWLANDeviceInterfaceSdio
  └ IOSDIOIoCardDevice              (IOSDIOFamily-27: CMD5, CCCR/CIS parse, personality match)
    └ AppleS5L8920XIOPSDIO          (AppleS5L8920X-227.1; IONameMatch sdio,s5l8920x)
      ├ IOP ring 3 "sdio"           (the SDIO task in the IOP firmware drives the SDHC)
      └ SDHC block at 0x80000000    (standard SDHCI register map, IRQ 0x26; the AP maps it too)
```

- **Transport:** the IOP ring, not AP-driven SDHCI.
  - `AppleS5L8920XIOPSDIO` sends 512-byte commands on ring 3: opcode at +0,
    status at +8, arguments from +0xc and +0x14.
  - Its strings name the operations: `initIOPState`, `resetController`,
    `resetCard`, `setClockRate`, `setBusWidth`, `sendCommand`,
    `sendDMACommand` / `startDMA`, and `freeIOPState`.
  - IOP firmware task 0x1524 (../research/gap-iop-mailbox-protocol.md §3) has
    opcodes 1 to 7:
    - 1: ping
    - 4: init
    - 5: reset and clock
    - 6: command
    - 7: data transfer, with {cmd, arg, blkSize, blkCount, segments} at
      +0x14…
    - 2 and 3: unconfirmed; 3 frees a buffer
- **SDHC registers, AP side:** the kext maps the SDHC block itself and has a
  standard-SDHCI register dumper (0xc0585250). The offsets it reads are the
  SDHCI spec's: +0x00 SDMA address, +0x04/06 block size and count, +0x08
  argument, +0x0c/0e mode and command, +0x10..1c response, +0x24 present state,
  +0x28 host control and the interrupt status and enable block. Its interrupt
  path (`interruptOccurred`, "slot = %d, src = %p, count = %d, normal = 0x%X,
  err = 0x%X") reads normal and error interrupt status on the AP. So the
  card interrupt (SDIO function interrupt, `normal` bit 8) almost certainly
  arrives on AP IRQ 0x26 from these registers.
- **Where the emulator stops today:** `s5l8930_iop.c` answers only
  `SDIO_OP_PING`. Boot log:

  ```
  AppleS5L8920XIOPSDIO::initIOPState(): IOP Command failed, invalid argument: status = 2 (0x2)
  AppleS5L8920XIOPSDIO::startSlot(): Failed to init IOP State for slot 0, invalid argument!
  ```

  Wi-Fi therefore fails cleanly. Nothing else depends on it.
- **DT `sdio` node** (7B500 DT and the real unit's IORegistry, hw2/regs/ioreg-full.txt):
  - `reg` 0x0/0x1000, which is 0x80000000
  - `interrupts` 0x26, `clock-gates` 0x30, `clock-ids` 0x129
  - `dma-channels` 3 @ 0x80000020
  - `function-device_reset` = TCA6408 expander (phandle 0x920970) pin 1, active high.
    The expander is already modelled (`s5l8930.tca6408`). `wlan` is expander pin 3
    in `event_name-gpio3`.
  - No `function-power_enable`. The kext looks for one and logs if it's missing.
    Nothing on the unit suggests that's fatal.
  - `local-mac-address` is **empty in the IPSW DT and filled by iBoot** from syscfg.
    The unit's value is `<unit Wi-Fi MAC>`. Without it the driver stops with
    "Unit isn't properly provisioned (no WiFi MAC Address)!", so `ipad1_kboot.py`
    has to fill it in (a fake locally-administered MAC is fine).
  - `wireless-board-snum` is also iBoot-filled (`J5024U3A9YXA` on the unit) and
    probably cosmetic.
- **Host-wake:** no separate GPIO. Card interrupts come in-band through the SDHC.
  (Bluetooth is the same BCM4329 combo chip on UART3 with its own `bt_reset`/`bt_wake`,
  and is independent of this. See ../research/bt-keyboard.md.)

## 2. What the driver matches and uploads

The real unit (hw2/regs/ioreg-full.txt):

```
IOSDIOIoCardDevice  IOSDIOProductInfo0 = "s=B1"  IOSDIOProductInfo1 = "P=K48 m=u80"   Cmd5Iterations = 1
AppleBCMWLAN        personality "K48 USI X17B - 4329 B1"   vendor-id = "USI"
                    NVRAM File = bcm94329OLYMPICX17UB.txt
                    STA Firmware = 4329b1/sdio-ag-cdc-full11n-reclaim-roml-wme-nocis.bin
                    IOMACAddress = <unit Wi-Fi MAC>  CountryCode = XZ2   SDIOClockLimit = 51.3 MHz
IO80211Interface    en0, SSID "<home SSID>", ch 6, 2.4 GHz
```

- **Matching:**
  - The CIS manufacturer tuple must be `0x02d0` (Broadcom) / `0x4329`.
  - The CIS version strings must contain `s=B1` and `P=K48 m=u` to get the K48
    USI personality. There's also a K48 Murata one; either works.
  - A bare `0x2d0/0x4329` falls back to "4329 Default" (N18 vars) with a
    "matched the default driver personality" warning. That would probably work
    too, but match K48 properly.
- **NVRAM vars** are not a file on disk. They come from the personality's
  `BCMWLANVars` string in the kext's Info.plist (`devid=0x432e`, `boardtype=0x509`,
  `xtalfreq=37400`, …). The driver writes them to the end of dongle RAM. The model
  only has to accept them.
- **Firmware** isn't on the filesystem either.
  - `/usr/libexec/wifiFirmwareLoader` is a launchd `RunAtLoad` job (source:
    `AppleBCMWLAN-2.60/FirmwareLoader`). It carries three images in its
    `__data` (784 KB binary):
    - `4325b0/…` 4.216.83.0
    - `4329b1/sdio-g-…` 4.218.175.1
    - `4329b1/sdio-ag-cdc-full11n-reclaim-roml-wme-idsup-nocis-pno-aoe-pktfilter-minioctl-x17-keepalive`
      4.218.175.43, the K48 one
  - It hands the matching image to the kext through `AppleBCMWLANUserClient`
    (`WiFiUserClientDownloadFirmware`), and the kext writes it over SDIO
    function 1.
  - The image is Cortex-M3 code for the dongle. **We never run it**: the model
    accepts the download into backplane RAM, which the driver reads back to
    verify, then plays the booted dongle itself, exactly as the iPod model does.
- **"idsup" means the in-dongle supplicant.** The kernel also has an
  `RSNSupplicant`. An open (no-security) network needs neither, so WPA is out of
  scope.
- **No ssh needed.** Everything above came from the IPSW rootfs, the kernelcache
  and the IORegistry dump already captured from the unit. If one fact needs the
  live unit later, it is `ioreg -l` of `IOSDIOIoCardDevice` for the raw CIS tuple
  bytes (`IOSDIOManufacturerTuple` did not serialise in the capture).

## 3. The minimum device model

Top to bottom, with what already exists:

| Layer | Needed | Exists? |
|---|---|---|
| IOP ring 3 | Opcodes 4/5 (init, reset, clock, bus width: status 0), 6 (command: CMD0/5/3/7/52 → R4/R6/R5 responses into the result words), 7 (CMD53 with scatter DMA to and from AP physical segments), 3 (free). Opcode 2: TBD. | Ping only (`s5l8930_iop.c`) |
| SDHC IRQ regs @ 0x80000000 | Normal/error interrupt status, enable and signal enable, present state; card-interrupt bit → IRQ 0x26; RW1C clear. Everything else can read as zero. | No |
| SDIO card, function 0 | CCCR, CIS with manufacturer 0x2d0/0x4329 and VERS_1 strings "s=B1", "P=K48 m=u80"; INT_PENDING derived from function 2 | iPod model (4325 values) |
| Function 1 backplane | Window register, SBSDIO clock CSR, chipcommon chip ID (0x4329, rev B1), core enumeration, SOCRAM download + readback, OTP read (the driver reads OTP for provisioning, `parseOTP`), SDIO core mailbox | iPod model (4325 layout) |
| Function 2 SDPCM | SDPCM frames, 12-byte CDC control channel, 6-byte BDC data channel, events on the data channel with OUI 00:10:18 | iPod model; the header sizes were measured against the 3.1.3 driver and need re-checking for 2.60 |
| Dongle ioctls | Success plus zero for the init set; `ver`, `cur_etheraddr`; `iscan` then a delayed `WLC_E_SCAN_COMPLETE`, then `iscanresults` with one `wl_bss_info_t` (open, 2.4 GHz); `WLC_SET_SSID` → `WLC_E_SET_SSID`/`LINK`/`ASSOC`; `WLC_GET_BSSID`/`BSS_INFO`; deepsleep | iPod model (all of it, including a synthetic open BSS and autojoin) |
| Data path | BDC-framed Ethernet ↔ `-netdev` (slirp) | iPod model (`qemu_new_nic` on netdev `wifi0`) |

How much of the dongle protocol is needed: **no 802.11 at all.** Association is
asserted by events. There's no WPA, no 5 GHz and no roaming. On the iPod, this set
was enough for a DHCP lease, mDNS and a page in Safari, so that is the whole list.

The chip-specific numbers to pin from the kext before writing code:

- The chip-ID and revision encoding the 4329 ChipManager accepts as "B1" (it logs
  "Unknown/Unsupported chip ID").
- Which cores it enumerates and where.
- SOCRAM size, which also sets the vars offset.
- Whether 2.60 changed the CDC or BDC header or the event dispatch table from
  the 3.1.3 driver's.

## 4. Prior art

- **In tree:** `hw/arm/ipod_touch_sdio.c` + docs/networking.md. It's a BCM4325
  dongle driven by 3.1.3's `AppleBCM4325` through the S5L8900 SDIO host, to
  association, DHCP and Safari. `tests/ipod/regress.py --checks wifi` guards it.
  Lines 1-878 are the card and dongle; 879+ are CMD execution and that SoC's host
  controller. The split we need is already visible in the file.
- **Public:** nothing that emulates the dongle side of a BCM43xx.
  - Linux `brcmfmac` and `bcmdhd` are host drivers. They're useful as the spec
    for SDPCM, CDC/BCDC, `wl_bss_info_t` and `WLC_E_*`, and the iPod model
    already follows them.
  - QEMU has a generic SDHCI and SD-bus, but no SDIO card model and no Broadcom
    device.
  - Corellium emulates iPhone Wi-Fi commercially and hasn't published it.
  - iEmu never reached SDIO.
  - openiBoot's `wlan.c` is S5L8900-only and stops after the firmware upload.

## 5. Plan and estimate

Each stage ends with a boot-log or UI check on a golden-pristine clone. The iPod's
regress `wifi` check has to stay green from stage 2 on.

| Stage | Work | Done when | Effort |
|---|---|---|---|
| 0. MAC | `ipad1_kboot.py` fills `sdio/local-mac-address` (+ `wireless-board-snum`) | selfcheck; DT dump | ½ day |
| 1. Enumerate | Reverse IOPSDIO ops 2-7 argument layout (kext 0xc0584000, 24 KB). Implement them in `s5l8930_iop.c` against a card behind an SDIO-card API; add the SDHC IRQ register block. | `IOSDIOIoCardDevice` with `s=B1`/`P=K48 m=u80`, `AppleBCMWLAN::start` on the K48 personality | 3-5 days |
| 2. Share the card model | Move the card and dongle half of `ipod_touch_sdio.c` into a chip-parametrised model (4325/4329: IDs, CIS, cores, RAM). The iPod host keeps its register interface. | iPod regress `wifi` still associates | 2-3 days |
| 3. Firmware up | 4329 chipcommon/cores/SOCRAM, OTP, download + verify, dongle-ready mailbox. wifiFirmwareLoader runs unmodified. | "BCM4329 revision B1", firmware version logged, `IO80211Interface` en0 attached, Settings shows "Not Connected" | 2-4 days |
| 4. Scan | Re-verify CDC/BDC/event framing against 2.60; `iscan` → delayed scan-complete → `iscanresults` | a fake open SSID in Settings > Wi-Fi | 1-3 days |
| 5. Join | `SET_SSID` → SET_SSID/ASSOC/LINK events, `GET_BSSID`/`BSS_INFO`, status-bar bars | "Connected", en0 up | 1-2 days |
| 6. DHCP | BDC data ↔ slirp netdev (`-netdev user,id=wifi0`), `wifi=on` machine property | lease on the wire, en0 has 10.0.2.x | 1 day |
| 7. Safari | DNS + HTTP through slirp; deepsleep and dim-screen path survives | page loads; survives a screen-off cycle | 1-2 days |

**Total: about 12-20 working days**, which is 2.5-4 calendar weeks with boot-test
cycles. It's front-loaded in stage 1 (the only real reverse engineering) and stage
3 (chip identity).

**Risks:**
- The ring command layout for ops 2, 6 and 7 is inferred, not proven. That
  unknown is stage 1's to close.
- 2.60 may have moved a header or event number. That shows up as a silent drop,
  so the iPod doc's list of silent failures is the first thing to check.
- Refactoring the shared card model touches a working iPod feature, which is
  why regress `wifi` gates stage 2.

**Value over USB Ethernet:** apps and configd see a real Wi-Fi interface. That
means reachability flags, the status-bar Wi-Fi icon, Settings > Wi-Fi and Maps'
Wi-Fi location path. The stock stack runs with no guest changes and no usbmuxd
dependency.
