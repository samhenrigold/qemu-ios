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

App install, 2026-10-04 (FirmwareKit kboot `n81ap-8C148` device, guest package with gles-public 81c8124a35 as an
offer): `regress.py --machine iPod-Touch-4G --checks app --guest-package OFFER` PASS. AppSync installs the harness through
installation_proxy, installd lists it, the agent launches it frontmost, and its GL triangle draws through the bridge
with no refusals.

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
| 6.0 | 10A403 | PASS | PASS | PASS | PASS | PASS | PASS | PASS | PASS |
| 6.0.1 | 10A523 | PASS | PASS | PASS | PASS | PASS | PASS | PASS | PASS |
| 6.1 | 10B144 | PASS | PASS | PASS | PASS | PASS | PASS | PASS | PASS |
| 6.1.2 | 10B146 | PASS | PASS | PASS | PASS | PASS | PASS | PASS | PASS |
| 6.1.3 | 10B329 | PASS | PASS | PASS | PASS | PASS | PASS | PASS | PASS |
| 6.1.5 | 10B400 | PASS | PASS | PASS | PASS | PASS | PASS | PASS | PASS |
| 6.1.6 | 10B500 | PASS | PASS | PASS | PASS | PASS | PASS | PASS | PASS |

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

### iOS 6

Every 6.x build (6.0-6.1.6) is prepared by FirmwareKit (`firmwarekit create` with the catalog's n81ap entries,
LightTouchMac fw-a4), and each device passed every gate above, run alone. 6.0 also passed by hand. Activation:
10A403 and 10A523 by the data ark, 10B144 onward by FirmwareKit's development-activation pattern. Two gate
flakes on a loaded host passed on a rerun: an AFC start during the install, and it_boot's console line coming after the
lock screen (the boot check now waits for it). On 6.0.1, SpringBoard drops the first tap on the app's icon after the
panel relights, three runs out of three. app-install taps twice (debt: the first touch after backboardd re-bootloads the
multitouch device).

What 6.x needed, all generic (nothing is chosen by build):

1. **boot_args Version** read off xnu-2107's pc-relative check (`ipad1_kboot.boot_args_version`).
2. **DRAM's first page aliased at physical 0** under kboot: the kernel copies its exception vectors to
   kvtophys(gPhysBase) = 0 after unmapping V=P.
3. **NVRAM image in `chosen/nvram-proxy-data`.** iBoot fills it; IODTNVRAM walks its partitions by
   length, so the zero placeholder hung the boot silently after the FIPS POST. kboot writes an empty CHRP
   image instead: a "common" partition and the rest free.
4. **Activation by data, not a patch.** 6.x lockdownd has no development shortcut. It reads its state from its
   data ark, and a factory unit carries `com.apple.mobile.lockdown_cache-ActivationState = FactoryActivated`
   there. That plist goes into `/var/root/Library/Lockdown/data_ark.plist` (`ipad1_rootfs build --lockdown DIR`;
   FirmwareKit does this when no binary strategy matches), and lockdownd stays stock.
5. **Content-protected data volume**: `kHFSContentProtectionBit` in the data volume's header (a restore sets
   it); 6.x installd's protection classes fail without it.
6. **AppSync**: 6.x installd also wants the signing identifier and entitlements in the MIS info. AppSync
   reads both from the executable's embedded signature.
7. **GL**:
   - The 914-field dispatch record has no field names. The front end names its slots by decoding the stock
     trampolines in the shared cache (a4-boards' scheme, shared with N90).
   - The macro context comes through `getMacroContextPrivate`.
   - The front end exports 6.x's eleven new names: APPLE_sync is answered, and the two others are stubbed.
8. **Power-off**:
   - The gesture rests at the end of the track before lifting; iOS 6 reads a moving lift as a flick back.
   - The CS42L59's halt handler waits for power-down done (0x38 bit 3) with no timeout. Before that bit was
     modelled, PEHaltRestart's 30 s watchdog panicked first.
9. **Wi-Fi**: the CDC ioctl length word is split (reply buffer low 16 bits, request high 16).

### iOS 6.0 beta 1 (10A5316k, June 2012)

Prepared by FirmwareKit (LightTouchMac guestdev-n81-6b1, catalog row n81ap-10A5316k, `prerelease` beta 1). The IPSW
(SHA-1 3b8101d3c30453e4c4ab8eae224e6a3d7f9f6a92, 846,900,415 bytes; Apple hosted it only behind the developer login) is
inside BetaArchive's `media_ipsw.rar` in archive.org's `Apple_iPod_Firmware` item ("Apple iPod Touch 4.1 Firmware 6.0
(6.0.10A5316k) (beta)", RAR SHA-1 ce93a767ca3655acfb86b22b25d1c47b9aae6c58; `bsdtar -xf` unpacks it). theapplewiki's
SundanceVail 10A5316k (iPod4,1) keys decrypt every component (`firmwarekit verify-keys`: 18 of 18), and BuildManifest
and Restore.plist name 10A5316k and iPod4,1.

| boot | usbmux | afc | persist | install + launch | GL app | power-off |
|---|---|---|---|---|---|---|
| PASS (activated home screen, Setup walked) | PASS | PASS | PASS | PASS | not run (ca_ogl off) | PASS |

What the beta needed:

1. **Expiry.** The check is lockdownd's, not SpringBoard's: when MobileGestalt's ReleaseType is "Beta"
   (SystemVersion.plist), it compares time() with a constant in the binary (10A5316k: 2012-07-18 00:00:01 UTC,
   also reported as lockdown's BuildExpireTime). Past it, it stops honouring the cached FactoryActivated: the data
   ark becomes `Unactivated` with BrickState true, and that sticks. FirmwareKit's own keybag and seal boots run
   lockdownd, so the clock is pinned for every boot: the machine property `rtc-epoch=<unix seconds>` starts the
   D1815's counter there at power-on, and the guest agent's clock sync (op 0x165) answers the same pinned time
   (it_agent otherwise sets the host's date within seconds). The recipe's `rtc_epoch` (1339848000,
   2012-06-16 12:00 UTC) goes into every one-shot boot and the lock's `machine`, which the app and regress.py
   append. The home screen's Calendar reads Saturday 16.
2. **Activation**: the data-ark FactoryActivated path, unchanged from 10A403, once the clock is pinned. No
   registered UDID is needed offline.
3. **GL front end**: the beta's OpenGLES exports `glMapBufferRangeAPPLE` and `glFlushMappedBufferRangeAPPLE`
   (renamed ...EXT by 10A403); both are in `opengles.exports` and route to the ...EXT rows. The beta's shared-cache
   OpenGLES carries no `__GLIFunctionDispatchRec` @encode, which the front end reads its macro-context layout
   from, so the row has `ca_ogl` off (software CoreAnimation) and no GL app was tried. Debt.
4. Fit-check differences from 10A403, both warnings: MobileStorageMounter names no UNSUPPORTED_FAILURE string
   (it_msmquiet left out), and the kernel's USB Ethernet classes differ (en1 unpinned). Setup Assistant walked as
   10A403's.

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
| Gyro (ap3gdl @0x68, INT1 0x21 / INT2 0x05) | ST L3G4200D (WHO_AM_I 0xd3): registers, 32-slot FIFO and its modes, INT2 levels, ODR timer; the device sits still (zero rate). The kboot DT's `gyro-sensitivity-calibration` (iBoot fills it from syscfg) gets a nominal identity at reset | new | R (calibration P) |
| Cameras / ISP | none; `camera=off` (default) unmatches the DT's `isp` node | absent (see debts) | P |

## Guest debugging

6.1.6 10B500: gdbstub + lldb with kernel and userland symbols PASS (`xnu-procs`; SpringBoard `mach_msg` stop with CoreFoundation frames). No debugserver: iOS ships none, and the 6.1 DeveloperDiskImage is not on this host. See [../guest-debug.md](../guest-debug.md).

## Debts

0. ~~**GL apps draw black (4.2.1).**~~ Fixed by gles-public 81c8124a35 (fw-a4 e9d82647fb). On these Retina
   boards a legacy 1x app's QuartzCore scales its EAGL surface with the M2 scaler: it queues
   `IOSurfaceAcceleratorConditionalTransferSurfaceWithSwap` and releases it by accelerator ID through
   `-[EAGLContext sendNotification:forTransaction:onLayer:]`, which the stock SGX engine turns into a kernel
   signal. The front end now rebinds that call, records CA's transfer and issues it unconditionally
   (`TransferSurfaceWithSwap`) after `glFinish`. `regress.py --checks app` (with the package as an offer) installs
   the harness, launches it and taps "GL: rotating triangle": the triangle draws (4 colours, 35 bridge lines) and the
   bridge refuses nothing.

1. **iBoot and the real NAND boot.** Only `kboot=` runs. A real N81 boots LLB/iBoot from NAND (boot
   blocks, `IOFlashPartitionScheme`) and keeps nvram/effaceable there. The NOR graft is the shortcut.
2. **NAND geometry** is K48's 16 GB part (`k48-16g`). The N81 SKUs are 8/32/64 GB, and its chip and DT
   values are unmeasured.
3. ~~**No FirmwareKit recipe.**~~ `n81ap-8C148` has a catalog entry and a kboot recipe (LightTouchMac branch
   `a4-n81`, experimental).
4. **Cameras.** No ISP model. AppleH3CamIn loaded the ISP CPU's firmware and then timed out on its mailbox
   a dozen times per boot (mediaserverd's sensor detection). The machine's `camera` property (default off) now
   unmatches the kboot DT's `isp` node at reset, the way `baseband` does, so the board reads as camera-less:
   no H3CamIn lines, and Camera.app opens to its closed shutter without crashing (N90 8C148, 2026-10-05).
   A real ISP model (the ISP CPU running its firmware, the sensors on i2c/MIPI) is what `camera=on` waits for.
5. ~~**Gyro**~~ (2026-10-05): AppleAP3GDL attaches and streams. `contrib/it-gyro` (spawned through the agent)
   reads CoreMotion at about 100 Hz, `gyroAvailable 1`, every rate 0. Left: a host input for rotation rates (the
   attitude path moves only the accelerometer), INT1's threshold events, the temperature byte, and the unit's
   real sensitivity matrix.
6. ~~Accelerometer mounting~~ (2026-10-04): the board's `accel_mount` "-2,-1,3" is the DT's orientation
   matrix inverted. Safari turns with `accel-orientation` 1/3 as on hardware (3 = Home right).
7. ~~**Multitouch calibration**~~ (2026-10-05): `mt_profile_n81`'s frame is now fitted the way the K48's was
   (x = -69 + 4748u, y = -215 + 7360v; the N1's y runs bottom-up). The iPod's frame had put taps 9 px off at the
   edges and 21-27 px high. `tests/ipad1/touchcal.py` taps a 4x5 grid on a Safari page that marks each touch:
   all 20 taps land within 1.5 px on both N90 and N81 8C148. The sensor's rows, columns and surface are still
   the iPod's.
8. **Panel ID** is K48's (nothing reads it on `kboot=`; iBoot will).
9. **LM48557 amp and audio out** are unverified (the codec driver starts).
10. **Power-off takes over 25 s** to `RB_HALT` (launchd waits on jobs). The machine's watch warning fires
    early; the harness waits for `guest-shutdown-confirmed`.
11. **Unlock timing.** The lock screen darkens about 8 s after waking. The harness's first slide can land
    on a dark panel after usbmux attaches; its retry handles it.
