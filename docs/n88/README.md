# iPhone 3GS (N88AP, S5L8920): `-M n88`

The second board on the S5L8920/8922 machine (`hw/arm/s5l8920.c`, docs/n18/README.md for the shared
models). The N88's board data: board-id 0, five UARTs (the bq27540 HDQ gauge on uart4, as the iPad's), its
own SPI NOR on spi0 (no graft), the spi2 baseband controller (nothing on it yet), i2c0 accelerometer, AK8973
compass, CS42L61 (the CS42L58 register model, as on the iPad) and CD3272. iOS 4.2.1 (8C148a) boots by kboot
from a NAND store to an activated home screen, with touch, Home and Hold, powers off cleanly and keeps its
data; an IPA installs through installation_proxy and AppSync and launches.

## What runs (2026-10-05)

| Build | Boot | Touch, Home, Hold | Power-off + persist | IPA install + launch | GL app |
|---|---|---|---|---|---|
| 3.1.3 7E18 | activated home screen ("No Service") | yes | yes (gesture, 15 s; marker survives) | yes (app-install.py all PASS; icon on page 2) | draws |
| 4.0 8A293, 4.0.1 8A306, 4.0.2 8A400, 4.1 8B117 | activated home screen | yes | yes | yes (app-install.py all PASS) | draws (debt 1) |
| 4.2.1 8C148a | activated home screen ("No Service") | yes | yes | yes (Harness) | renders through the bridge (readback PASS); on the panel at the wrong stride (debt 1) |
| 4.3 8F190, 4.3.1 8G4, 4.3.2 8H7, 4.3.3 8J2, 4.3.4 8K2, 4.3.5 8L1 | activated home screen | yes | yes | yes (app-install.py all PASS) | draws (debt 1) |
| 5.0 9A334, 5.0.1 9A405, 5.1 9B176, 5.1.1 9B206 | Setup Assistant, walked to the home screen; GL-composited | yes | yes | yes (Harness) | draws (debt 1) |
| 6.1.6 10B500 | userland: lockdown answers 6.1.6, SpringBoard runs, the panel stays on the Apple logo (no GL front end for 6.x yet, so prepared with ca_ogl off; debt 9) | | | | |

- kboot (`imgtools/s5l8920_kboot.py n88 --nor`, the DT's own NOR kept) -> xnu-1504.58.28
  RELEASE_ARM_S5L8920X. The kernel's EmbeddedIOP firmware is the s5l8920x build of iBoot-931 (the N18 runs
  the s5l8922x build), and it drives the H2FMI its own way: FMC at +0x400 and ECC at +0x800 inside the DT's
  4 KiB window, READ ID read a byte at a time (go 0x10 after 0x90), a blank page reported by ECC summary
  bit 6, and every transfer started by a control write of its own (`explicit-start`, below). Chips identify
  (0xB614D5AD on both buses), VFL opens on an epoch-3 store (`ipad1_nand.py --epoch 3`, the IPSW's SCEP),
  the YaFTL R/O restore takes seconds, BSD root disk0s1, fsck clean, launchd.
- Data protection: effaceable storage and NVRAM on the N88's NOR, formatted with the system keybag by the
  restore-ramdisk one-shot (`ipad1_keybag.py --board n88`, ramdisk 038-0082-001); kb_load passes.
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

Assets under `~/Developer/qemu-ios-files/n88/` (never committed): the 8C148a IPSW, its keys page, the
`ipad1_fw.py` output in `dec/`, `identity.json`, `mbr.bin` (as on the N18).

Device with AppSync, the GL front end and the activation hook (`contrib/appsync/build.sh`,
`contrib/gles-public/build.sh` and `build-apps.sh` first):

```
F=~/Developer/qemu-ios-files/n88
imgtools/ipad1_rootfs.py build --rootfs $F/dec/rootfs.dmg --pristine $F/dec/rootfs.dmg --mbr $F/mbr.bin \
    --out $F/userland-gl --lockdown none --stash none --no-usb-net --no-web-proxy --no-ca-ogl --appsync --gles
python3 -c "import sys; sys.path.insert(0, 'imgtools'); import ipad1_rootfs as r, os
d = '$F/userland-gl/pristine'
with r.Mounted(d + '/system.img', d + '/mnt-system') as m:
    r.activation_hook('$HOOK', os.path.join(m.mnt, r.LOCKDOWND))"    # HOOK: offline-activation-8C148/patch_lockdownd.py
imgtools/ipad1_nand.py build --no-whitening --epoch 3 --geometry k48-16g --mbr $F/mbr.bin \
    --kernelcache $F/dec/kernelcache.mach --system $F/userland-gl/pristine/system.img \
    --data $F/userland-gl/pristine/data.img --out $F/userland-gl/nand
imgtools/s5l8920_kboot.py n88 --identity $F/identity.json --nor $F/dec $F/kboot-nor.bin \
    "serial=3 debug=0x8 -v amfi_allow_any_signature=1 cs_enforcement_disable=1"
mkdir $F/dev4; cp -cR $F/userland-gl/nand $F/dev4/nand
python3 -c "open('$F/dev4/nor.bin','wb').write(b'\xff'*0x100000)"
imgtools/ipad1_keybag.py $F/dev4/nand $F/dev4/nor.bin --dec $F/dec --ramdisk 038-0082-001-ramdisk.dmg \
    --identity $F/identity.json --board n88
build/qemu-system-arm -M n88,kboot=$F/kboot-nor.bin,nand=$F/dev4/nand,nand-overlay=OV,nor-rw=NORCOPY \
    -display none -serial file:serial.log -qmp unix:/tmp/n88.qmp,server,nowait
tests/ipad1/app-install.py --machine n88 --device $F/dev4 --kboot $F/kboot-nor-nov.bin --nor $F/dev4/nor.bin \
    --product-version 4.2.1 --ipa Harness.ipa --gl-tap 0.5,0.165 --out $F/runs/app
```

(`ipad1_rootfs.py bake` instead of the bare hook also adds the guest tools and package, and disables
BTServer; it needs `contrib/ipad1-guest/build.sh` and `contrib/guest-package/build.sh`.) The writable NOR holds effaceable and NVRAM: give each run its own copy. The
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
| I2C0 | D1755 0x74, CS42L61 0x4a (CS42L58 model), CD3272 0x39, LIS302DL 0x1d, AK8973 0x1e | board data | H |
| I2C2 | TSL2561 0x49 (`s5l8930.tsl2561`, a TSL2581 layout variant; no threshold interrupt) | board data | H |
| UART3 | BCM4325 HCI (`ipod_touch_bt.c`'s chardev); the CDMA receive chain from URXH is paced by the UART's FIFO | board data | H |
| ISP | none: the DT's `isp` node is unmatched (`no_isp`), as the A4 machines' | board data | S |

`explicit-start`: the s5l8920x firmware leaves FMI control at 3 or 5 between transfers and starts each
with its own control write. The last page of a multi-page read follows a status poll, not a read command,
and a write's FIFOs fill while control is still 5 from the previous page, before the next CE is selected.
With the property a control 3 takes any CE whose latched page no transfer has taken, and only a control-5
write arms a write transfer. Without it, every 8-page READ_MULTIPLE timed out (2 s, status 0x8000001c, the
last page's meta never sent) and single-page writes were captured against the stale CE and dropped
("Failed Index read" panic). `IOP_RING_TRACE=1` compares every READ/WRITE_MULTIPLE page with the store.

## Gates run (2026-10-05, branch n88 after merging ipad1 at 235d0d170b)

N88 app-install.py (install, launch, GLES through the bridge, power-off) PASS on 3.1.3, 4.2.1 and 5.1.1
(Setup walked); 3.1.3 power-off gesture after the panel slept, then the afcclient marker read back after a
reboot: PASS. Shared-model checks: iPad 7B500 `tests/ipad1/regress.py --checks boot,persist,gles` PASS;
iPod 2G `tests/ipod/regress.py --checks boot` PASS; N81 `regress.py --machine iPod-Touch-4G --checks boot`
PASS; N18 unlock to the home screen with touch PASS.

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
   wake latch, Wi-Fi.
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
9. **iOS 6**: FirmwareKit (a4-n81 6233871: boot_args version 3, nvram-proxy-data filled) plus PA 0 as DRAM's
   first page get 10B500 through the keybag one-shot and into userland (system_mib 1536). The GL front end
   (contrib/gles-public) does not fit 6.x yet (imports NSObject classes no 6.x image exports), so the device is
   prepared with ca_ogl off, and SpringBoard then has no context to draw into ("CGContext... invalid context
   0x0"): the panel keeps the boot logo. Waits on the 6.x GL shim (a4-boards).
