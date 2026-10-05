# iPod touch 4G (n81ap, S5L8930 "A4"): `-M iPod-Touch-4G`

The second board on the A4 machine (`hw/arm/ipad1.c`): an `A4Board` (`a4_n81`) next to the iPad's
`a4_k48`, and a machine type that subclasses `ipad1-machine`, so every property, QMP command and app bridge
the iPad has applies here too. iOS 4.2.1 (8C148) boots from a direct kernel bundle (`kboot=`) through
root mount and launchd to SpringBoard. Activation is offline, as on the iPad. The lock screen and home
screen draw at 640x960 through the GL bridge, and touch works through the real N1 digitizer driver.
Wi-Fi joins `qemu-ios`. usbmux, AFC and lockdown answer (DeviceClass iPod). A clean power-off persists
the NAND. Nothing runs through iBoot yet.

## What runs

- Kernel entry from `imgtools/ipad1_kboot.py`. The board comes from the DT's own compatible
  (`N81AP` -> `BOARDS["n81"]`): 640x960 framebuffer, `display-rotation` 0, `display-scale` 2,
  board-id 8, model MC540.
- No SPI NOR on this board (`boot-from-nand`: nvram, syscfg and effaceable live in NAND). The kboot
  editor grafts K48's 4.x `spi0/nor-flash` subtree in instead (`graft_nor`, with `DeviceTree.add_node`):
  diagnostic-data, nvram, raw-device and `effaceable,nor`. It also renames `boot-from-nand` out of the
  way, because the kernel keys off the property's presence, not its value. With it present,
  IOFlashStorageDevice hunts NAND boot blocks and the FTL never finds root. The machine's NOR model is
  unchanged.
- Data protection: `/defaults content-protect` makes launchd demand a system keybag ("FATAL KEYBAG ERROR:
  kb_load", then a reboot into restore mode). `imgtools/ipad1_keybag.py` makes one exactly as for the
  iPad 4.x: one boot of the IPSW's restore ramdisk with `it_keybag` formats effaceable (in the grafted
  NOR) and writes `/var/keybags/systembag.kb`. It boots the board's own machine.
- Touch: AppleMultitouchN1SPI (`multi-touch,n18`; `n90` too). It speaks the Z2 HBPP/report protocol of
  `ipod_touch_multitouch.c` and loads Common.mtprops' N1F55 firmware, 53196 bytes in one DMA burst.
  That burst needed the SPI's `tx-fifo-depth` (the board sets 64 KiB; the default stays 50000 for the
  iPod/iPad migration stream). The N1 driver's two strobes (19 c1 wake, ee ee after EXECUTE) are
  answered. The profile is `mt_profile_n81`: family 0x55, bcdVersion 0x0079.
- Display: the shared S5L8930 pipe at the board's `width`/`height`, DSI 4 lanes. The portrait panel
  needs no touch transform (`touch_landscape` false).
- Power-off: `system_powerdown` is the user's gesture, with the knob measured off the 8C148 sheet
  (116,134 -> +460, the same in every orientation since SpringBoard is portrait-only). launchd calls
  `reboot(RB_HALT)` and `guest-shutdown-confirmed` turns true. It takes longer than on the iPad: the
  machine's 25 s warning fires before the halt.
- I2C: D1815 PMU (IRQ GPIO 0x0d) and the CS42L59 codec (CS42L58 register file) on i2c0; LIS331DLH and
  TSL2581@0x49 on i2c2. Battery is the PMU's alone (no gauge, no chargers).
- BCM4329 as `P=N81` (AppleBCMWLAN's "N81 - 4329 B1" personality, n81.bin 4.221.38.1), synthetic MAC.

Verified 2026-10-04 on `a4-boards`:
- `tests/ipad1/regress.py --machine iPod-Touch-4G` with `boot,persist,afc,usbmux,wifi`: all PASS.
- A tap on the power-off sheet's Cancel dismisses it.
- The home screen: `~/Developer/qemu-ios-files/n81/home-8C148.png`.

## How to boot

Assets (never committed) live in `~/Developer/qemu-ios-files/n81/`: the IPSW (Apple's, sha1
6a890696...), `keys.txt` (from api.ipsw.me) and the outputs below. Since FirmwareKit has no n81
recipe yet, a device is the iPad 4.x pipeline run by hand:

```
N=~/Developer/qemu-ios-files/n81
imgtools/ipad1_fw.py $N/iPod4,1_4.2.1_8C148_Restore.ipsw $N/keys.txt $N/dec
python3 -c 'import sys; sys.path.insert(0,"imgtools"); import ipad1_kboot as k, json; \
  print(json.dumps(k.synth_identity("n81-8C148-default", "16g", "n81")))' > $N/identity.json   # mode 600
imgtools/ipad1_kboot.py --identity $N/identity.json $N/dec $N/kboot.bin
imgtools/ipad1_nand.py mbr --geometry k48-16g --system-mib 1280 $N/mbr.bin
imgtools/ipad1_rootfs.py build --rootfs $N/dec/rootfs.dmg --pristine $N/dec/rootfs.dmg --mbr $N/mbr.bin \
  --out $N/userland --stash none --lockdown none
imgtools/ipad1_rootfs.py bake $N/userland/pristine --activation-hook ACTIVATION_HOOK   # patch_lockdownd.py
imgtools/ipad1_nand.py build --geometry k48-16g --mbr $N/mbr.bin --kernelcache $N/dec/kernelcache.mach \
  --system $N/userland/pristine/system.img --data $N/userland/pristine/data.img --out $N/userland/nand-pristine
mkdir $N/dev1; cp -cR $N/userland/nand-pristine $N/dev1/nand
python3 -c 'open("'$N'/dev1/nor.bin","wb").write(b"\xff"*0x100000)'
imgtools/ipad1_keybag.py $N/dev1/nand $N/dev1/nor.bin --dec $N/dec --ramdisk 038-0024-002-ramdisk.dmg \
  --identity $N/identity.json
```

The activation hook is the iPad 8C148 one (`qemu-ios-files/ipad1/offline-activation-8C148/patch_lockdownd.py`).
The iPod's lockdownd is byte-identical to the iPad's (same SHA-256), and the hook finds its branch by
pattern. The build needs `contrib/gles-public/build.sh`, `contrib/ipad1-guest/build.sh` and
`contrib/guest-package/build.sh` first.

Boot it (the writable NOR keeps effaceable, so give each run its own copy):

```
build/qemu-system-arm -M iPod-Touch-4G,kboot=$N/kboot.bin,nand=$N/dev1/nand,nand-overlay=OV,nor-rw=OV/nor.bin \
  -display none -serial file:serial.log -qmp unix:/tmp/n81.sock,server,nowait
```

Gates: `tests/ipad1/regress.py --machine iPod-Touch-4G --device $N/dev1 --kboot $N/dev1/kboot.bin --nor
$N/dev1/nor.bin --checks boot,persist,afc,usbmux,wifi`. The machine selects the scanout size for touch
coordinates, the unlock slider, the lit threshold (the plugged-in iPod's lock screen is the charging battery
on black, about 30% lit) and the DeviceClass.

## Firmware coverage (fw-a4, 2026-10-05)

Every iPod4,1 build from api.ipsw.me, prepared by the pipeline under "How to boot" with three generic
additions, then run through the gates below. Keys: api.ipsw.me's key pages (`keys/ipsw/iPod4,1/BUILD`),
rendered into the key-page text `ipad1_fw.py` reads; an entry whose key is "0" is a key nobody published.

- **Activation**: FirmwareKit's pattern-based recognizer (LightTouchMac `Packages/FirmwareKit/Sources/CActivation/
  activation.c`, built as its standalone CLI) is the `--activation-hook`; it finds the development shortcut in
  every 4.x/5.x lockdownd. No per-build hook.
- **Keybag on 4.3.1-4.3.5**: no ramdisk keys are published, so the keybag one-shot boots 8F190's Update ramdisk
  (the same sibling rule as FirmwareKit's `keybag_ramdisk_from`); `ipad1_fw.py` skips the unkeyed ramdisks.
- **NAND signature epoch**: `ipad1_nand.py build` writes NANDDRIVERSIGN's '0' + PE_nand_epoch read off the kernel
  (2 from 4.3.5's IOFlashStorage 410.4).

Gates (each run alone; `tests/ipad1/regress.py --machine iPod-Touch-4G --device D --kboot K --nor N`):
`boot` (lit, unlock, Hold locks; 5.x: lockdown Activated and Setup Assistant's first page), `usbmux`, `afc`,
`persist` (guest power-off, marker survives), `wifi`; then `tests/ipad1/app-install.py` with the same
arguments: Setup Assistant walked on 5.x, the Harness IPA installed through installation_proxy (AppSync),
its icon put on page 1 through springboardservices, launched, its GLES 1.1 row tapped (cyan/magenta fixture on
the panel, readback PASS, no bridge refusals), guest power-off.

| iOS | Build | boot | usbmux | afc | persist | wifi | install + launch | GL app | power-off |
|---|---|---|---|---|---|---|---|---|---|
| 4.1 | 8B117 | PASS | PASS | PASS | PASS | PASS | PASS | PASS | PASS |
| 4.1 | 8B118 | PASS | PASS | PASS | PASS | PASS | PASS | PASS | PASS |
| 4.2.1 | 8C148 | PASS | PASS | PASS | PASS | PASS | PASS | PASS | PASS |
| 4.3 | 8F190 | PASS | PASS | PASS | PASS | PASS | PASS | PASS | PASS |
| 4.3.1 | 8G4 | PASS | PASS | PASS | PASS | PASS | PASS | PASS | PASS |
| 4.3.2 | 8H7 | PASS | PASS | PASS | PASS | PASS | PASS | PASS | PASS |
| 4.3.3 | 8J2 | PASS | PASS | PASS | PASS | PASS | PASS | PASS | PASS |
| 4.3.4 | 8K2 | PASS | PASS | PASS | PASS | PASS | PASS | PASS | PASS |
| 4.3.5 | 8L1 | PASS | PASS | PASS | PASS | PASS | PASS | PASS | PASS |
| 5.0 | 9A334 | PASS | PASS | PASS | PASS | PASS | PASS | PASS | PASS |
| 5.0.1 | 9A405 | PASS | PASS | PASS | PASS | PASS | PASS | PASS | PASS |
| 5.1 | 9B176 | PASS | PASS | PASS | PASS | PASS | PASS | PASS | PASS |
| 5.1.1 | 9B206 | PASS | PASS | PASS | PASS | PASS | PASS | PASS | PASS |
| 6.0 | 10A403 | kernel stops after corecrypto (below) | - | - | - | - | - | - | - |
| 6.0.1-6.1.6 | 10A523, 10B144, 10B146, 10B329, 10B400, 10B500 | not run (6.0 first) | | | | | | | |

Notes: the boot check's unlock slide can miss on a loaded host (debt 11): every boot above passed when run
with no other emulator or preparation running (two batch runs needed that rerun; one app-install tap on 8B117 missed the icon once and passed on the rerun). By hand, a build is
the pipeline in "How to boot" with `--activation-hook` the activation.c CLI and, on 4.3.1-4.3.5, 8F190's
decrypted Update ramdisk copied into the build's `dec/` for `ipad1_keybag.py --ramdisk`.

What it took beyond the 8C148 bring-up:

1. **1x apps on the 2x panel went through the M2 scaler.** 4.x/5.x CoreAnimation with an ES2 render server
   queues `IOSurfaceAcceleratorConditionalTransferSurface[WithSwap]` and hands the token to GL
   (`CAEAGLContextScalarNotification` -> `sendNotification:<accelerator ID>`), which the SGX would release. The
   GL front end (`contrib/gles-public`) records those calls and issues the unconditional transfer from a worker
   thread when the notification arrives. Without it every 1x app's CAEAGLLayer was black.
2. **Scaler version** (+0x260) 0x20002 on the A4: 4.3's driver refuses tiled buffers at version 0.
3. **Scaler byte loads**: 5.x's reset polls with `ldrb`.
4. **CLCD ENVID set at reset**: 5.x's AppleCLCD adopts only a running panel (else the Apple logo stays up).
5. **Setup Assistant** (5.x) walked by label (OCR of the upright panel); the bottom edge of the digitizer reads
   a little high (debt 7), so a link there is retapped lower.

### iOS 6: not booting yet

10A403 (6.0) gets through `pe_identify_machine` (boot_args Version 3 read off xnu-2107's pc-relative check) and,
with the SecureROM window mapped at physical 0 under kboot (the kernel copies its exception vectors to
kvtophys(gPhysBase) = 0 after unmapping V=P), through corecrypto's FIPS self-test; it then stops printing and
the panel keeps the boot logo, the CPU busy and the timer ticking (6.1.6 10B500 the same). Not yet diagnosed;
ruled out by experiment: the NOR graft, the SGX DT override, `arm-io/clock-frequencies`, filling
`chosen/nvram-proxy-data` with an empty CHRP image, a 64-byte `chosen/random-seed`. Its lockdownd has no development shortcut; the hactivation
path (`should_hactivate`, MobileGestalt `ShouldHactivate`) is the candidate for a recognizer strategy.

## Models: reused, varied, new

Classes as in LightTouchMac `docs/fidelity-ledger.md`: R register-level, H high-level emulation, P a
documented quirk/patch, S stub.

| Component | Model | How | Class |
|---|---|---|---|
| SoC (VICs, PMGR, GPIO, CDMA, DART, H2FMI/IOP NAND, SHA1, PKE, USB, I2S, AMC, display pipe, DSIM) | the iPad's S5L8930 models | shared, as-is (PMGR `board-id` 8, display 640x960) | as on K48 |
| NOR | the iPad's SPI NOR, grafted into the DT | P (the board has none) | R |
| Multitouch | Zephyr2/N1 model, `mt_profile_n81`, SPI `tx-fifo-depth` 64 KiB | variant | H |
| PMU D1815, LIS331DLH, TSL2581 | shared | as-is (TSL2581 at 0x49) | H |
| Codec CS42L59 | the CS42L58 register file | shared | H |
| BCM4329 | the iPod's dongle model, `P=N81`, n81.bin version | variant (board data) | H |
| Gyro (ap3gdl / mpu3100 @0x68) | none: both probes fail and AppleEmbeddedI2CGyro frees itself | absent | - |
| Cameras / ISP | none | absent (see debts) | - |

## Debts

1. **iBoot and the real NAND boot.** Only `kboot=` runs. A real N81 boots LLB/iBoot from NAND (boot
   blocks, `IOFlashPartitionScheme`) and keeps nvram/effaceable there. The NOR graft is the shortcut.
2. **NAND geometry** is K48's 16 GB part (`k48-16g`). The N81 SKUs are 8/32/64 GB, and its chip and DT
   values are unmeasured.
3. **No FirmwareKit recipe.** The device above is the iPad pipeline run by hand. An `n81ap-8C148`
   catalog entry and recipe are needed for the app.
4. **Cameras.** AppleH3CamIn times out on its ISP mailbox several times per boot. Disable the camera nodes
   in the DT or stub the ISP.
5. **Gyro** absent (Game Center/CoreMotion users see no gyro). An ID-register stub at 0x68 is next.
6. **Accelerometer mounting** untested. The DT's orientation matrix differs from K48's
   (`0000ff00 000000ff 00010000`), and `accel_flipped` is off.
7. **Multitouch calibration**: rows/columns/surface are the iPod 2G's, not measured on a unit. Taps land
   where aimed on the sheet's buttons, but the edges have not been fitted the way the K48's frame values were.
8. **Panel ID** is K48's (nothing reads it on `kboot=`; iBoot will).
9. **LM48557 amp and audio out** are unverified (the codec driver starts).
10. **Power-off takes over 25 s** to `RB_HALT` (launchd waits on jobs). The machine's watch warning fires
    early; the harness waits for `guest-shutdown-confirmed`.
11. **Unlock timing.** The lock screen darkens about 8 s after waking. The harness's first slide can land
    on a dark panel after usbmux attaches; its retry handles it.
