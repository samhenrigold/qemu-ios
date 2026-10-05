# iPhone 4 GSM (n90ap, S5L8930 "A4"): `-M iPhone-4`

The third board on the A4 machine (`hw/arm/ipad1.c`, `a4_n90`). It shares almost everything with N81
(`../n81/README.md`): the portrait 640x960 Retina panel, the N1 digitizer, kboot only, and K48's NOR grafted
into the DT. It adds 512 MiB of DRAM, the iPad's CS42L61 codec and Mikey, the HDQ gas gauge (bq27540,
1420 mAh), Bluetooth on uart3, and a baseband. iOS 4.2.1 (8C148) boots to an activated lock screen
("No Service") and the home screen. Touch, Wi-Fi, usbmux/AFC (DeviceClass iPhone), and clean power-off
with persistence all work.

## What runs

- 512 MiB: the machine's DRAM and its iBoot mirror come from the board. CDMA's memory-vs-FIFO test reads
  the `dram-size` property (default 256 MiB), and the IOP core's DRAM window follows its `dram` link.
  `ipad1_kboot.py` puts memSize, vram and pram at the top of 512 MiB (`BOARDS["n90"]["dram"]`).
- Baseband: the DT keeps its `baseband` node (`compatible baseband,n90`). The machine's `baseband`
  property (default off) rewrites that node's `compatible` to `none` in the staged kboot DT at every
  reset, so nothing matches it and nothing waits on a silent radio. The cell stream's modem turns it back on
  with `baseband=on`. spi2 (BasebandSPI, IFX protocol v2, MRDY/SRDY GPIOs) is the stock controller with
  nothing on the bus; its model belongs to the cell stream. With the node unmatched, CommCenter runs and
  the status bar shows "No Service".
- Wi-Fi: the BCM4329 CIS reads `s=B1` / `P=N90 V=u`, so AppleBCMWLAN takes its "N90 USI - 4329 B1"
  personality (n90.bin 4.221.38.1, bcm94329OLYMPICN90U.txt). With an empty P= it fell back to the default
  personality and never downloaded firmware. It joins `qemu-ios` and DHCPs.
- Touch: `multi-touch,n90`, the N1 driver and Common.mtprops' N1F55 firmware, exactly as on N81
  (`mt_profile_n81`, `mt_tx_fifo` 64 KiB).
- Activation: the 8C148 offline-activation hook from the iPad and N81. The iPhone's lockdownd is the same
  binary (SHA-256 60597aa3...).
- Power-off: N81's knob (SpringBoard is portrait-only). The halt comes 18-40 s after the request, so the
  board's watch is 60 s (`pwroff_watch_ms`).
- I2C: PMU (0x74, IRQ 0x0d), CS42L61 (CS42L58 register file, 0x4a), AK8973 (0x1e, the DT's `compass`
  node), Mikey (0x39) on i2c0; LIS331DLH (0x19) on i2c2.

Verified 2026-10-04: `tests/ipad1/regress.py --machine iPhone-4 --device DEV --checks
boot,persist,usbmux,afc,wifi`, all PASS.

App install, 2026-10-04 (FirmwareKit kboot `n90ap-8C148` device, guest package with gles-public 81c8124a35 as an
offer): `regress.py --machine iPhone-4 --checks app --guest-package OFFER` PASS. AppSync installs the harness through
installation_proxy, installd lists it, the agent launches it frontmost, and its GL triangle draws through the bridge
with no refusals.

## iOS 5.1.1 (9B206)

Verified 2026-10-05 on the kboot pipeline (`~/Developer/qemu-ios-files/n90/fw/prepare.sh 9B206`, the N81 by-hand
pipeline with n90 paths; activation through fw-a4's `lt_activate` hook; guest package serial 15):
- `regress.py --machine iPhone-4 --checks boot`: PASS. Activated, Setup Assistant's first page.
- `usbmux`, `afc`, `persist`: PASS.
- `wifi`: PASS run alone. In the four-boot run it timed out waiting for a lease once.
- `tests/ipad1/app-install.py --machine iPhone-4`: all PASS. AppSync install; walks iOS 5's Setup Assistant
  ("Set Up as New iPhone" ... "Start Using iPhone"); launch; the Harness GL fixture on the panel (50% fixture
  colours, no bridge refusals, Harness "PASS GLES pixel readback" and "PASS GLES framebuffer/draw/present API");
  guest power-off.
- The fixes it took were in the harness, not the models. The panel can sleep during the install, so the walk
  wakes it before sliding. The syslog relay closes mid-run on 5.x, so the Harness report is also read from its
  own Documents/results.log over house_arrest.
- Without a camera node, CLTM logs "could not find camera service" every 5 s (`camera=off`).

## iOS 6.1.3 (10B329) and 7.1.2 (11D257): what breaks (2026-10-05)

1. Fixed: boot_args.Version. 6.x reaches pe_identify_machine's "Epoch Mismatch" string with movw/movt,
   so ipad1_kboot read Version 0 and the kernel panicked before the console came up. Now 3 (08a698c2f1).
   FirmwareKit's KBoot.swift has the same scan (LightTouchMac a4-n81 6233871).
2. Fixed: PA 0. In early init both kernels ml_io_map PA 0 (ml_vtophys of gPhysBase, no longer V=P) and copy
   the reset and exception vectors there. The machine had nothing at PA 0, so the copy took an external abort
   ("sleh_abort at interrupt context"). Past the boot ROM, PA 0 is now an alias of DRAM's first page, which
   these kernels leave out of the image (they link at 0x80001000).
3. Fixed: NVRAM. iBoot-1537/1940 hand NVRAM to the kernel as `/chosen/nvram-proxy-data` (8 KiB, which N90
   has no NOR for). The IPSW DT reserves it zeroed, and IODTNVRAM::initNVRAMImage loops forever on a
   zero-length partition. That was the busy CPU after AppleKeyStore. ipad1_kboot now fills it with an empty
   CHRP image (2 KiB "common", the rest "free"). 7.1.2 then runs IOKit through Wi-Fi, USB and the N1
   multitouch.
4. 6.1.3 now prepares end to end, keybag one-shot included, and boots to launchd and SpringBoard, but the panel
   stays dark. backboardd, 6.x's render server, crash-loops in the GL front end:
   - First, QuartzCore sends `-[EAGLContext getMacroContextPrivate]` (5.x's selector was GetMacroContextPrivate).
     It is answered now.
   - Then gles-public finds no dispatch layout on 6.x ("dispatch layout from none: 0 slots"). There is no
     `__GLIFunctionDispatchRec` @encode in the 6.x shared cache, and the trampoline decoder doesn't know 6.x's
     OpenGLES. So the macro context's table is empty and QuartzCore jumps through NULL during the IOMFB swap.
     This is the 6.x GL dispatch work.
   - There is no guest-package family for 10*, so no it_boot or agent.
5. 7.1.2 needs a 1664 MiB system partition (`SYSMIB=1664`; its rootfs is about 1.5 GB). fw-a4's lt_activate
   fails on 7.1.2's lockdownd, so the device is baked without activation (`prepare.sh 11D257 none`).
   - The reboot loop right after /private/var mounted was the bake's rw root fstab. 7.x's launchd cannot
     `mount -uw /` (mount_hfs: Operation not permitted), so it reboots. `ipad1_rootfs.py build --ro-root`
     (`RO_ROOT=1 prepare.sh`) keeps the stock ro root; launchd then runs, Unactivated.
   - Found by baking /.launchd_log_shutdown and /.launchd_log_debug and reading /var back off the NAND.
   - Then backboardd crash-loops on the same GL dispatch gap as 6.1.3 ("dispatch layout from none: 0 slots";
     SIGSEGV at 0 in QuartzCore).
6. Fixed: the 6.x GL dispatch layout. The 6.x and 7.x shared caches carry `{__GLIFunctionDispatchRec=^?^?...}`
   with no field names: 914 slots on 6.1.3. gles-public now takes the count from that @encode and names the
   slots by decoding the stock OpenGLES's exported trampolines, found through its symbol table in the cache's
   shared __LINKEDIT. The front end replaces OpenGLES, so dlsym would only find its own exports.
   - The decoder needed two 6.x shapes. The TLS context load (`ldr r0, [r0, #0x78]`) is not the GC, and the
     float trampolines keep the context in sb (r9).
   - 301 of 914 slots are named, and CoreAnimation's calls land on them.
   - 6.1.3 `regress.py --checks boot` now PASSES: Activated, Setup Assistant's first page on the panel, GL
     bridge refused nothing. backboardd's console is not SpringBoard's, so the GL-path line is also taken from
     the host's log.
7. 7.1.2 with `RO_ROOT=1` and the shim boots to iOS 7's Setup "Hello" screen and its language list, drawn
   through the GL bridge: 1035 slots, 316 named.
   - CoreAnimation fences every frame with glFenceSyncAPPLE (slot 779, 1024 calls before Setup).
   - APPLE_sync is now in the name table as ids 912-918, exported by 6.x and 7.x OpenGLES. The front end answers
     it locally: the host finishes every call before the next, so a fence is signalled as it is made.
   - The bridge refuses nothing on 7.1.2.
   - Open: regress's Setup walk for iOS 7 (the slide-to-set-up gesture and the new pages), activation
     (lt_activate fails on 7.1.2's lockdownd), a guest package for 10*/11*, and the 6.x Wi-Fi lease.
8. 6.1.3 Wi-Fi: the driver comes up ("setupDriver(): Succeeded") and joins, with the split CDC length and
   "cap" handled. With wlan.log.level=7, the DHCP offers are seen arriving ("Rx ... UDP sport 67 dport 68").
   - A boot with usbmuxd attached got its lease (10.0.2.15 at 26 s).
   - regress's wifi check boots without USB. There, two of two runs never took an offer, and the NetManager's
     10 s IP window ran out.
   - Resolved: the guest did lease. 6.x logs the lease with two spaces ("receivedIPv4Address():  Received"),
     and under host load the NetManager window expired after the lease. The check now matches either spacing
     and also accepts a DHCP ACK in the slirp pcap. 7.x also needed the full 52-byte WL event header
     (`WL_EVENT_MSG_LEN`).

## iOS 6.0-6.1.3: results (2026-10-05)

FirmwareKit `n90ap-10A403`, `-10A523`, `-10B144`, `-10B146` and `-10B329` (catalog entries on LightTouchMac a4-n81
51f27b1). Each was created fresh, then `tests/ipad1/app-install.py --machine iPhone-4` ran on it:
mux, AppSync install, lock, the Setup walk, icon, launch, GL (about 50% fixture colours, Harness "PASS GLES pixel
readback", no bridge refusals) and guest power-off all PASS on every build.
- Two fixes from fw-a4 were needed: AppSync answering 6.x's MIS signing-identity and entitlement keys, and the
  content-protection bit on the data volume.
- The harness retaps the GL fixture once. The first tap is sometimes dropped while the app is still settling.
- 6.1.3 by hand (`prepare.sh 10B329`) passes the same run.

## How to boot

The same hand pipeline as N81 (`../n81/README.md`, "How to boot"), with `n90` paths and
`synth_identity(seed, "16g", "n90")`. Assets are in `~/Developer/qemu-ios-files/n90/`. Apple's IPSW
iPhone3,1_4.2.1_8C148 has md5 93957e7b...; its real SHA-1 is 366b28e9..., not the b8b485c1... api.ipsw.me lists.

```
build/qemu-system-arm -M iPhone-4,kboot=$N/kboot.bin,nand=$N/dev1/nand,nand-overlay=OV,nor-rw=OV/nor.bin \
  -display none -serial file:serial.log -qmp unix:/tmp/n90.sock,server,nowait
```

Gates: `tests/ipad1/regress.py --machine iPhone-4 --device $N/dev1` (the device's `device.lock.json` says
`boot_strategy kboot`, so its kboot.bin and nor.bin are used), and `tests/fresh-device.sh n90ap-...` once
FirmwareKit has the recipe.

## Models

As N81, plus:

| Component | Model | How | Class |
|---|---|---|---|
| DRAM 512 MiB | board `dram_size`; CDMA `dram-size`; IOP core window from its `dram` link | variant (board data) | R |
| HDQ gas gauge (uart5) | the iPad's bq27545 model, 1420 mAh | variant | H |
| CS42L61 + Mikey | the iPad's | shared | H |
| Baseband (spi2, GPIOs) | DT node unmatched by `baseband=off`; the controller with nothing on it | P (until the cell stream's modem) | S |
| Compass | AK8973 at the DT's 0x1e node (the unit has AK8975B at 0x0c/0x0d) | variant | H |

## Debts

0. ~~**GL apps draw black (4.2.1).**~~ Fixed by gles-public 81c8124a35 (fw-a4 e9d82647fb). On these Retina
   boards a legacy 1x app's QuartzCore scales its EAGL surface with the M2 scaler: it queues
   `IOSurfaceAcceleratorConditionalTransferSurfaceWithSwap` and releases it by accelerator ID through
   `-[EAGLContext sendNotification:forTransaction:onLayer:]`, which the stock SGX engine turns into a kernel
   signal. The front end now rebinds that call, records CA's transfer and issues it unconditionally
   (`TransferSurfaceWithSwap`) after `glFinish`. `regress.py --checks app` (with the package as an offer) installs
   the harness, launches it and taps "GL: rotating triangle": the triangle draws (4 colours, 35 bridge lines) and the
   bridge refuses nothing.

1. **Baseband** waits on the cell stream (spi2 IFX protocol, modem core). Today the node is unmatched and
   the device shows "No Service".
2. **Absent parts**:
   - ~~The gyro~~: the L3G4200D model, as on N81 (its debt 5).
   - The ALS/prox ct700 (i2c0 0x29): AppleCT700 logs "Probing hardware failed" and stays out, so there is
     no auto-brightness and no proximity blanking on calls. A model is a TAOS-style register file (command
     byte 0x80|reg) plus `als-calibration`/`prox-calibration`, which iBoot fills from syscfg and the driver
     checks for a signature and limits. Deferred (2026-10-05) until calls want proximity.
   - The Highland Park voice processor (AUD10: i2c0 0x3e, uart6, i2s2) passes probe and start with nothing
     behind it. Its message protocol (firmware download, routing, algorithm parameters) is only exercised
     on a call's audio route. Deferred, since calls carry no audio.
   - The GPS (bcm4750 on uart4).
   - The cameras and ISP. `camera=off` (default) unmatches the DT's `isp` node, as on N81 (its debt 4).
3. **Compass**: the AK8973 stands in for the AK8975B pair.
4. ~~Accelerometer mounting~~ (2026-10-04): `accel_mount` "-2,1,-3" (the DT orientation's transpose).
   Safari turns with `accel-orientation` 1/3/4 as on hardware.
5. All of N81's debts apply: no iBoot or NAND boot, K48's 16 GB NAND geometry, K48's panel ID. Touch is
   calibrated (N81's debt 7, the shared `mt_profile_n81`).
