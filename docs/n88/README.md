# iPhone 3GS (N88AP, S5L8920): `-M n88`

The second board on the S5L8920/8922 machine (`hw/arm/s5l8920.c`, docs/n18/README.md for the shared
models). The N88's board data: board-id 0, five UARTs (the bq27540 HDQ gauge on uart4, as the iPad's), its
own SPI NOR on spi0 (no graft), the spi2 baseband controller (nothing on it yet), i2c0 accelerometer, AK8973
compass, CS42L61 (the CS42L58 register model, as on the iPad) and CD3272. iOS 4.2.1 (8C148a) boots by kboot
from a NAND store to an activated home screen, with touch, Home and Hold, powers off cleanly and keeps its
data; an IPA installs through installation_proxy and AppSync and launches.

Wi-Fi (2026-10-08): the board's BCM4325 D1 (AppleBCMWLANBusInterfaceSDIO's "N88 - 4325 D1": CIS s=D1 / P=N88,
manufacturer 0x2d0, product 0xa8f2, the production card's) behind the SDHC at 0x80000000, the same card model as
the N18's at the iPod 2G card's chip ID and SDIO core. Before that the N88 had no card at all: iOS 6's AppleIOPSDIO
read the unimplemented window ("SDIO Internal Clk Unstable", clockControl 0) and Settings showed Wi-Fi off. Every
build from 3.0 to 6.1.6 joins "qemu-ios" (sessions single's wifi0 probe on 3.1, 3.1.2, 3.1.3, 4.2.1, 5.1.1, 6.1.6;
the serial log's "Joined BSS" on 3.0, where the probe's httpget does not run). tests/qtest/s5l8920-sdio-test checks
the card. What 3.x needed:
- Product 0xa8f2: 3.0's AppleBCM4325-42.43 matches the N88 only by it ("N88 4325 D1"; 0x4325 needs its DVT boards'
  "P=N88 m=3.1"/"m=3.2"), so with 0x4325 no driver attached. 3.1 to 6.1.6 match either.
- The VXD and 3.1.x Wi-Fi: 3.1.x's AppleBCMWLANN88PlatformManager::init waits 20 ms (waitForMatchingService) for
  AppleBaseband, which registers at the end of its start. That start sat ~1.2 s in addEventSource on arm-io's shared
  work loop, whose gate AppleVXD375's power-on held while polling an unmodeled decoder (DMAC 0x50c FIN 1000 x 1 ms,
  MTX 0xfc, the firmware's 0x2fe0 signature), so the card enumerated first: "Failed to get Baseband service", no
  Wi-Fi. kboot unmatched the `vxd` node until the decoder had a model; `s5l8920.vxd` answers that power-on at once, so
  the node is matched again and 3.1.3's Wi-Fi comes up with it (sessions single on fk-7E18). Found with gdbstub
  breakpoints on IOService::probeCandidates/startCandidate, IORecursiveLockLock/Unlock on the work loop's gate and
  IODelay callers.

## What runs (2026-10-05)

| Build | Boot | Touch, Home, Hold | Power-off + persist | IPA install + launch | GL app |
|---|---|---|---|---|---|
| 3.0 7A341 (3.0.1 7A400 untried) | activated home screen (software CoreAnimation; FirmwareKit n88ap-7A341, with the modem) | yes | yes (gesture; marker survives) | yes (app-install.py all PASS with the legacy-linked Harness, `contrib/it-harness --flavor ios2`; icon on page 2) | no (ca_ogl off) |
| 3.1 7C144, 3.1.2 7D11 | activated home screen | yes | yes (gesture; marker survives) | yes (app-install.py all PASS; icon on page 2) | draws |
| 3.1.3 7E18 | activated home screen ("No Service") | yes | yes (gesture, 15 s; marker survives) | yes (app-install.py all PASS; icon on page 2) | draws |
| 4.0 beta 1 8A230m | activated home screen | yes | yes (gesture; marker survives) | yes (app-install.py all PASS; icon on page 2) | draws |
| 4.0 8A293, 4.0.1 8A306, 4.0.2 8A400, 4.1 8B117 | activated home screen | yes | yes | yes (app-install.py all PASS) | draws (debt 1) |
| 4.2.1 8C148a | activated home screen ("No Service") | yes | yes | yes (Harness) | renders through the bridge (readback PASS); on the panel at the wrong stride (debt 1) |
| 4.3 8F190, 4.3.1 8G4, 4.3.2 8H7, 4.3.3 8J2, 4.3.4 8K2, 4.3.5 8L1 | activated home screen | yes | yes | yes (app-install.py all PASS) | draws (debt 1) |
| 5.0 9A334, 5.0.1 9A405, 5.1 9B176, 5.1.1 9B206 | Setup Assistant, walked to the home screen; GL-composited | yes | yes | yes (Harness) | draws (debt 1) |
| 6.0 10A403, 6.0.1 10A523 | Setup Assistant, walked to the home screen (with the modem: `baseband=on`, the lock's `imei=`) | yes | yes (gesture; marker survives) | yes (app-install.py all PASS) | draws |
| 6.1 10B141, 6.1.2 10B146, 6.1.3 10B329, 6.1.6 10B500 | Setup Assistant, walked to the home screen | yes | yes (gesture; marker survives) | yes (app-install.py all PASS) | draws |

- kboot (firmwarekit's KBoot, the DT's own NOR kept) -> xnu-1504.58.28
  RELEASE_ARM_S5L8920X. The kernel's EmbeddedIOP firmware is the s5l8920x build of iBoot-931 (the N18 runs
  the s5l8922x build), and it drives the H2FMI its own way: FMC at +0x400 and ECC at +0x800 inside the DT's
  4 KiB window, READ ID read a byte at a time (go 0x10 after 0x90), a blank page reported by ECC summary
  bit 6, and every transfer started by a control write of its own (`explicit-start`, below). Chips identify
  (0xB614D5AD on both buses), VFL opens on an epoch-3 store (the IPSW's SCEP),
  the YaFTL R/O restore takes seconds, BSD root disk0s1, fsck clean, launchd.
- Data protection: effaceable storage and NVRAM on the N88's NOR, formatted with the system keybag by the
  restore-ramdisk one-shot (firmwarekit's keybag step, ramdisk 038-0082-001); kb_load passes.
- Activated by the FirmwareKit lockdownd strategy (`offline-activation-8C148/patch_lockdownd.py` matches the
  N88's lockdownd). SpringBoard draws the lock screen; Home wakes panel and digitizer (the N88's firmware
  0x0066.bin downloads over HBPP, frames read), slide to unlock, home screen. Hold 4 s brings up "slide to
  power off" (`runs/poweroff-sheet.png`).
- Power-off: QMP `system_powerdown` (the machine's gesture) ends in "AppleNANDFTL::_powerDownHandler: sync
  complete" and QEMU's exit about 15 s after the request. A 70001-byte file pushed with `afcclient` over
  usbmuxd-qemu (`usb-tcp-addr=`) reads back identical after that power-off and a reboot on the same
  overlay; lockdown answers ProductVersion 4.2.1, ActivationState Activated.
- Apps: `tests/ipad1/app-install.py --machine n88` PASS on every step (mux, install, lock, icon, launch, gl,
  shutdown): the harness IPA goes in through installation_proxy and AppSync, is pinned to page 1, launches,
  its GLES row renders through the bridge (readback PASS, no refusals), then the guest powers off. The
  guest-services trap is the N18's (s5l8920 766a2b1647), machine-wide.
- Status bar: "No Service". The baseband (spi2, IFX v1) is the cellular stream's (`baseband=on`).
- 5.1.1 (2026-10-05, FirmwareKit-prepared `n88ap-9B206`): two model changes. The D1755's dock data lines read
  0 V while a USB cable is attached (pcf50633 `brick-mux`, channel 6), or 5.x's power source calls the cable
  "Detached" and the USB device stack never starts; and under kboot the CLCD reports iBoot's blend output
  (0x1b10 bit 0, 0x1b24 its size), which 5.x's AppleM2CLCD::start_hardware requires before it adds the
  framebuffer (else SpringBoard draws into no context). Setup Assistant walked by hand (English,
  Australia, Location off, new iPhone, Apple ID skipped, terms, diagnostics), the home screen composites
  through the GL bridge, the Harness installs and launches and its GLES row draws, and an AFC marker
  survives power-off and reboot.

## How to boot

Assets under `~/Developer/qemu-ios-files/n88/` (never committed): the 8C148a IPSW. LightTouchMac makes the
device, with AppSync, the GL front end, the guest package and the activation hook, and boots it (the
`imgtools/ipad1_*.py` and `tests/ipad1/app-install.py` recipe this listed is retired; see git history at
5508b504b8):

```
F=~/Developer/qemu-ios-files/n88
firmwarekit create --catalog CATALOG --id n88ap-8C148a --ipsw $F/iPhone2,1_4.2.1_8C148a_Restore.ipsw --out $F/dev
swift run --package-path tests/sessions sessions single $F/dev      # in LightTouchMac
```

The writable NOR holds effaceable and NVRAM: give each run its own copy. The
lock screen powers the digitizer down after a few idle seconds: press Home before a drag. A tap is a press
and release under ~0.1 s; a slower one is a long press (icons wiggle).

Machine properties as the N18's (docs/n18/README.md).

## Models: what differs from the N18

| Component | Model | How | Class |
|---|---|---|---|
| H2FMI | `s5l8930.h2fmi` with `fmc-offset=0x400` `ecc-offset=0x800` `ecc-blank-summary=0x40` `explicit-start` | variant | R |
| CDMA | as the N18; HOLD on a running channel reports held (bit 17), as every board's model now does | shared | R |
| Buttons | hold 0xb7, menu 0xb6 active high (DT flag 0x100, interrupt type 7: both edges), at rest after reset | board data | R |
| NOR | the N88's own spi0 NOR (DT node kept) | shared | R |
| UARTs | five; bq27540 HDQ gauge on uart4 (1219 mAh) | board data | H |
| SPI2 | baseband controller, no device | | S |
| GPS | the baseband's own receiver: `+XLSR` fixes from the modem's `gps-fix` (`bb_gps`; docs/baseband/gps.md) | board data | H |
| I2C0 | D1755 0x74, CS42L61 0x4a (CS42L58 model), CD3272 0x39, LIS302DL 0x1d, AK8973 0x1e (`compass-heading`; DT orientation 9: x and y swapped, x negated) | board data | H |
| I2C2 | TSL2561 0x49 (`s5l8930.tsl2561`, a TSL2581 layout variant; no threshold interrupt) | board data | H |
| UART3 | BCM4325 HCI (`ipod_touch_bt.c`'s chardev); the CDMA receive chain from URXH is paced by the UART's FIFO | board data | H |
| ISP | none: the DT's `isp` node is unmatched (`no_isp`), as the A4 machines' | board data | S |
| PWM (0x83500000) | `s5l8920.pwm` (`hw/arm/s5l8920_pwm.c`): channel 0 (DT pwm/vibrator) runs the vibration motor, which the app hears (`qemu_ios_ui_vibrator`, `hw/misc/ios_vibrator.c`); channel 2 is the codec MCLK, unwired | shared | H |
| VXD (H.264 decoder, 0x85000000, IRQ 0x2a) | `s5l8920.vxd`, the A4's model (docs/n81/README.md); 3.1.3 hands it Annex B slices. Movies show on the CLCD's NV12 video plane (6.1.6 NOVA's intros, n88ap-10B500) | shared | H |

`explicit-start`: the s5l8920x firmware leaves FMI control at 3 or 5 between transfers and starts each
with its own control write. The last page of a multi-page read follows a status poll, not a read command,
and a write's FIFOs fill while control is still 5 from the previous page, before the next CE is selected.
With the property a control 3 takes any CE whose latched page no transfer has taken, and only a control-5
write arms a write transfer. Without it, every 8-page READ_MULTIPLE timed out (2 s, status 0x8000001c, the
last page's meta never sent) and single-page writes were captured against the stale CE and dropped
("Failed Index read" panic). `-trace s5l8930_iop_ring_log` compares every READ/WRITE_MULTIPLE page with the store.

## Gates run (2026-10-05, branch n88 after merging ipad1 at 235d0d170b)

N88 app-install.py (install, launch, GLES through the bridge, power-off) PASS on 3.1.3, 4.2.1 and 5.1.1
(Setup walked); 3.1.3 power-off gesture after the panel slept, then the afcclient marker read back after a
reboot: PASS. Shared-model checks: iPad 7B500 `tests/ipad1/regress.py --checks boot,persist,gles` PASS;
iPod 2G `tests/ipod/regress.py --checks boot` PASS; N81 `regress.py --machine iPod-Touch-4G --checks boot`
PASS; N18 unlock to the home screen with touch PASS.

## Guest debugging

6.1.6 10B500: gdbstub + lldb with kernel and userland symbols PASS (`tests/ipad1/debug-check.py`). No debugserver: iOS ships none, and the 6.1 DeveloperDiskImage is not on this host. See [../guest-debug.md](../guest-debug.md).

## Debts

1. **GL scene on the panel**: as the N18's debt 8, the GLES view reaches the panel laid out at the panel's
   320-pixel stride (`runs/app/install/gl.png`); the readback is right. No it_agent on this machine.
2. **Baseband**: spi2 bare; "No Service" (cellular stream).
3. **Bluetooth**: uart3 answers the HCI. BlueTool powers the chip through CommCenter, so it comes up only with
   `baseband=on` (seen: boot script to the end, "Server attached", paired devices 0); without it BTServer
   exits and launchd respawns it every 10 s. FirmwareKit leaves BTServer Disabled unless the recipe sets
   `bluetooth` (set for 3.1.3 only: see 7).
4. **Camera**: none (the isp node is unmatched). 4.x gave up on the unanswered ISP mailbox after a few 2 s
   waits; 3.1.3's ISP_waitCommunicationEnd (0xc054d17c) resets its count and busy-waits forever.
5. As the N18: kboot with the NOR (no NAND boot blocks), K48 fuses and clock table, D1755 backlight and
   wake latch.
6. Every 4.x and 5.x build runs. The devices are FirmwareKit's
   (LightTouchMac n88-app catalog n88ap-*), prepared with a LightTouchDevice + this tree's libqemu-arm.dylib.
7. **3.1.3** runs (FirmwareKit-prepared, catalog n88ap-7E18 with `bluetooth`). What it took:
   - NAND board lookup: 3.1.3's FMI groups the CEs into buses by the disk node's `landing-map` words (one CE
     mask per bus; findNandInfo 0xc043c6da, read per bus by 0xc043b028). The IPSW's DT has a single word, so
     every CE folded into one bus and nothing in its board table matched ("2-bus not supported"); kboot now
     writes one word per populated bus from the disk's CE bitmap.
   - Store signature: 3.1.3 refuses NANDDRIVERSIGN flags > 4 ("Incompatible Signature", 0xc05c58aa). It is
     YaFTL on VSVFL as 4.x is; its own FIL signs flags 4 when it formats (seen by letting it format a blank
     XOR store with nand-enable-reformat=1, which needs the s5l8920x raw read, `cfg-v0`). Plain stores now
     sign 4 (4.x and 5.x accept it).
   - Touch worked all along. What looked like dead touch was the panel's power: SpringBoard sets MultitouchHID's
     UILocked when the display sleeps, and 3.1.3's MTDeviceSetUILocked powers the digitizer down for it
     (unless MultitouchSupport's NoPowerDownInUILock). Home woke the panel only ~20 s later: SpringBoard's wake
     path made four BTSessionAttach calls, each its full 5 s timeout, because BTServer (Disabled by the bake)
     never answered. With Bluetooth on (3 above) a Home press lights the panel at once and the power-off
     gesture completes. Found by breakpointing the kernel driver's log function (0xc030d20c), setPowerEnabled
     (0xc030d5b4) and mach_msg with timeouts over the gdbstub.
   - Also gone: the ISP busy-wait (4) and AppleTSL2561's failed enable (I2C2 0x49 had no device).
   - The gate: the IPA's icon lands on page 2 (3.x's page 1 is full) and 3.1.3 raises "Waiting for
     activation" after the install; app-install.py swipes and dismisses both alert shapes.
8. ~~**Touch calibration**~~: fitted (mt_profile_n88 frame_*): GSEventGetLocationInWindow's point read through
   the gdbstub (lldb) per tap, as the M68's; an 11-tap check lands within 1 px of every aim. The 5.x Setup walk
   now taps its links where they are drawn.
9. **iOS 6**: 6.1-6.1.6 pass, prepared by FirmwareKit. What it took beyond the A4 boards' 6.x work (boot_args
   version, nvram-proxy-data, PA 0 as DRAM's first page, fw-a4's GL front end and AppSync):
   - **/product/product-id**: iBoot fills it; the IPSW DT reserves it zeroed. 6.x GraphicsServices compares it
     (MobileGestalt's product hash, read from IODeviceTree:/product) with the 3GS's to choose CGFontCacheUR.plist,
     the only font set the 3GS IPSW ships. Zeroed, it chose CGFontCache.plist, whose _H_ fonts are not on the
     3GS: every UIFont and bitmap context was nil and Setup crash-looped (NSParagraphStyle nil). FirmwareKit's
     KBoot fills it as board data (the constant is GraphicsServices' own).
   - 6.0 and 6.0.1: their lockdownd keeps the data ark's FactoryActivated only once CommCenter says the device is
     a phone ("This device is a phone. It supports factory activation"), so they need the modem. FirmwareKit's
     keybag and seal boots now carry it for the radio boards (`baseband=on`, the lock's `imei=`, which the UDID
     hashes), so the sealed store keeps FactoryActivated; the app and regress boot the same modem.
10. **3.0** (fixed 2026-10-05; FirmwareKit n88ap-7A341 since): 3.0's yaFTL always takes one block-TOC page
    (YAFTL_Init 0xc05c74ec on 7A341 compares `data <= data*4*n`, so n stays 1; 3.1 fixed it). The k48-16g store's
    vendor type 0x100014 gives two VFL banks per CE, so 2048-page superblocks whose TOC needs two pages: the R/O
    restore read past the TOC and built a garbage map ("mismatch between lpn and metadata at lpn 0 meta -1").
    3.0's own AppleS5L8920XIOPFMI table (0xc041cf60) gives this part on 2 buses x 4 CEs vendor type **0x10001**, one
    bank per CE: 1024-page superblocks, whose TOC fits one page. `ipad1_nand.py --geometry k48-16g-v1` (since retired) built that
    store. (The table refuses 2+2 CEs, "2-bus not supported", and gives 1x4 0x100014, so a single-bus map does not
    help.) With it, 3.0 restores its context, mounts root and reaches an activated lock screen ("No Service"), and
    `regress.py --machine n88 --product-version 3.0` passes usbmux, afc (all five sizes) and persist (one run; the
    power-off gesture missed once at load ~50). The other 3.0 differences, as N18 3.1.x: `--sig-flags 4`, an
    unjournaled 8 KiB data volume, and the kboot DT guards (3.0's DT has no raw-panel-id or snum slots).
    `firmwarekit create --id n88ap-7A341` builds it (the Python recipe this listed is retired; git history at
    5508b504b8).

    The home screen draws in full: the status-bar-only frames were shots taken before SpringBoard loaded its icons.
    Boot it with the modem (`baseband=on`, as regress and the app do): without one, CommCenter's SPI reset loop starves
    SpringBoard and it ignores unlock, Home and Hold for minutes. Apps (2026-10-05): the guest package's `k48-ios30`
    family (n88ap 7A341/7A400) carries the iPad helpers, it_msmquiet and AppSync legacy-linked against the 3.1.3 SDK
    (3.0's dyld refuses LC_DYLD_INFO_ONLY), with armv7.itpack's `loader/it_boot-legacy`; FirmwareKit picks the
    `<name>-legacy` helpers where no executable of the firmware carries LC_DYLD_INFO_ONLY. 3.0 has no
    springboardservices lockdown service, so app-install.py reads the icon's slot from SpringBoard's saved iconState
    through the agent. `app-install.py --machine n88 --product-version 3.0 --ipa <ios2 Harness> --gl-tap ''`: mux,
    install, lock, icon, launch ("Harness 1.0 | iOS 3.0"), shutdown PASS.
