# iPod touch 3G (N18AP, S5L8922): `-M n18` (and the iPhone 3GS, `-M n88`)

The S5L8920/8922 machine (`hw/arm/s5l8920.c`): the A4's blocks (`s5l8930_*`) at this SoC's addresses and
interrupt numbers, the S5L8720's display, USB and Dialog PMU models, and two blocks of its own (PMGR,
`s5l8920_pmgr.c`, `s5l8920_dart.c`). iOS 4.2.1 (8C148) boots by kboot (no iBoot) from a NAND store to the
home screen, with touch, and powers off cleanly with its data intact. The iPhone 3GS (N88AP, S5L8920) is the
second board on the same machine file (`-M n88`, below).

## What runs (2026-10-04)

- kboot bundle (`imgtools/s5l8920_kboot.py n18`, ipad1_kboot's iBoot stand-in with the board's values) ->
  xnu-1504.58.28 RELEASE_ARM_S5L8922X -> every platform driver starts: VICs, GPIO IC, performance
  controller, IOP (the kernel's EmbeddedIOP firmware on the second core), NAND through the IOP and the
  H2FMI, SPI, I2C, D1755 PMU, CS42L58, M2 CLCD, MIPI-DSIM, SWI, scaler, USB PHY/OTG (device mode,
  "Connected to a USB host"), Zephyr multi-touch on SPI1 ("successfully started").
- Root: YaFTL mounts the store's MBR volumes ("Creating block device of 3925449 sectors"), BSD root
  disk0s1, fsck clean, /private/var on disk0s2, launchd.
- Data protection: effaceable storage and NVRAM on a SPI NOR grafted into the DT (debt 1), formatted with
  the system keybag by the iPad's restore-ramdisk one-shot (`ipad1_keybag.py --board n18`).
- Activated by the FirmwareKit lockdownd strategy (`offline-activation-8C148/patch_lockdownd.py`, the
  4.2.1 lockdownd is byte-identical to the iPad's), SpringBoard draws the lock screen through the M2 CLCD
  behind dart0. Home wakes the panel, slide to unlock works (N1F55 digitizer firmware downloaded, frames
  read), the home screen comes up (2026-10-04, `screens` in qemu-ios-files/n18/runs/t4.png).
- Power-off: QMP `system_powerdown` makes the user's gesture (Home, Hold 3.5 s, drag "slide to power
  off"); SpringBoard swaps every framebuffer, AppleM2TVOut's too, so TV-out is modelled; the guest unmounts,
  syncs the FTL ("AppleNANDFTL::_powerDownHandler: sync complete") and QEMU exits about 15 s after the
  request. Persistence (2026-10-05): a 70001-byte file pushed with `afcclient` over usbmuxd-qemu
  (`usb-tcp-addr=`) reads back identical after that power-off and a reboot on the same overlay; lockdown
  answers ProductVersion 4.2.1 on both boots.
- App install (2026-10-05): `tests/ipad1/app-install.py --machine n18` PASS on every step: the harness IPA
  goes in through installation_proxy and AppSync, gets pinned to page 1, launches, its GLES row renders
  through the bridge (readback PASS, no refusals), then the guest powers off. The guest-services trap carries
  GLES and guest packages (debt 8 covers what reaches the panel). Recipe below.
- Wi-Fi (2026-10-05): the board's BCM4329 B1 (AppleBCMWLAN's "N18 - 4329 B1": CIS s=B1 / P=N18,
  4329b1/n18.bin 4.221.38.1) as the iPad's dongle model, behind the SDHC at 0x80000000 (IRQ 0x22) that the
  IOP firmware's sdiodrv drives. The stock stack joins the open BSS "qemu-ios", takes 10.0.2.15 from slirp and
  shows the Wi-Fi icon. `wifi=` and `wifi-bssid` work as on the iPad (docs/ipad1/wifi.md);
  `regress.py --machine n18 --checks wifi` PASS. It needed the CDMA HOLD state (n88's 3aa02ab1a8): with
  Wi-Fi up, BTServer parks uart3's RX channel.
- LightTouchMac (2026-10-05, LTM branch n18-app): FirmwareKit prepares `n18ap-8C148` end to end. That covers
  the kboot board, a plain NAND store (the DT has no metadata-whitening), the keybag one-shot, the seal and the
  check boot. On that device, app-install passes all 7 steps and regress persist and wifi pass. The machine takes
  `die-id` as ipad1 does.
- Audio: I2S0 gets the codec's PCM from CDMA channel 0x15 on the audio clock (no listening test yet).
- Not yet: the D1755's button wake path (debt 6), a USB host port for regress's keyboard checks (debt 9).

## How to boot

Assets under `~/Developer/qemu-ios-files/n18/` (never committed): the 8C148 IPSW, its keys rendered as a
keys page, `ipad1_fw.py` output in `dec/`.

```
F=~/Developer/qemu-ios-files/n18
imgtools/ipad1_fw.py $F/iPod3,1_4.2.1_8C148_Restore.ipsw $F/keys-8C148.txt $F/dec
imgtools/ipad1_kboot.py --synth-identity n18-default $F/identity.json
imgtools/ipad1_nand.py mbr --geometry k48-16g --system-mib 1280 $F/mbr.bin
imgtools/ipad1_rootfs.py build --rootfs $F/dec/rootfs.dmg --pristine $F/dec/rootfs.dmg --mbr $F/mbr.bin \
    --out $F/userland --lockdown none --no-usb-net --no-web-proxy --no-ca-ogl
imgtools/ipad1_nand.py build --no-whitening --geometry k48-16g --mbr $F/mbr.bin \
    --kernelcache $F/dec/kernelcache.mach --system $F/userland/pristine/system.img \
    --data $F/userland/pristine/data.img --out $F/userland/nand-pristine
imgtools/s5l8920_kboot.py n18 --identity $F/identity.json $F/dec $F/kboot.bin \
    "serial=3 debug=0x8 -v amfi_allow_any_signature=1 cs_enforcement_disable=1"
mkdir -p /tmp/n18-ovl
build/qemu-system-arm -M n18,kboot=$F/kboot.bin,nand=$F/userland/nand-pristine,nand-overlay=/tmp/n18-ovl \
    -display none -serial file:/tmp/n18.log
```

`--no-whitening`: the N18/N88 DTs have no `metadata-whitening`, and WMR refuses a whitened store ("Metadata
whitening not supported"). The NAND geometry is the iPad's 16 GB Hynix part (the IOP firmware's chip table
knows it); the unit's own part is not modelled yet.

For data protection (4.x needs effaceable storage and a system keybag) build the kboot with `--nor`, make
a device from a copy of the store and an erased 1 MiB NOR, and run the one-shot on them; for the lock screen
to unlock, give lockdownd the activation strategy when building the system image (`ipad1_rootfs.py`'s
`activation_hook`, as `bake --activation-hook` does):

```
imgtools/s5l8920_kboot.py n18 --identity $F/identity.json --nor $F/dec $F/kboot-nor.bin "serial=3 debug=0x8 amfi_allow_any_signature=1 cs_enforcement_disable=1"
mkdir -p $F/dev; cp -cR $F/userland/nand-pristine $F/dev/nand
python3 -c "open('$F/dev/nor.bin','wb').write(b'\xff'*0x100000)"
imgtools/ipad1_keybag.py $F/dev/nand $F/dev/nor.bin --dec $F/dec --ramdisk 038-0031-002-ramdisk.dmg \
    --identity $F/identity.json --board n18          # needs build/ipad1-guest/it_keybag (armv7)
build/qemu-system-arm -M n18,kboot=$F/kboot-nor.bin,nand=$F/dev/nand,nand-overlay=/tmp/n18-ovl,nor-rw=$F/dev/nor.bin ...
```

The lock screen turns the panel off after a few idle seconds and the digitizer with it (the personality's
`DisablePowerForUILock`): press Home (`qom-set /machine button-home true`, then false) before a drag.

Machine properties: `kboot`, `nand`, `nand-overlay`, `nor`, `nor-rw` (on the N18 a NOR on spi0 only when one
is set), `button-home`, `button-hold`, `usb-tcp-addr` (usbmuxd-qemu's QEMU port, as on the iPad), `die-id`,
`wifi`, `wifi-bssid`, `guest-package`, `gles-debug`, `gles-rejects`, `display-sleeping` (the DSI panel is off),
`accel-orientation`/`-x`/`-y`/`-z`/`-shake`. The app's button bridge reaches the board's pins
(`s5l8920_press_button`). The guest agent (`agent-request`/`-result`/`-status`/`-cancel`, it_agent) and the
pasteboard work as on the iPad. `battery-level` (0-100, the D1755's ADC), `battery-charging` (auto/on/off) and
`usb-attached` work as on the iPod, at boot (-M) and at run time.

### App install

The system image needs AppSync and the GL front end (`contrib/appsync/build.sh`,
`contrib/gles-public/build.sh` and `build-apps.sh`). Add the activation hook, then build the store and the
keybag as above:

```
imgtools/ipad1_rootfs.py build --rootfs $F/dec/rootfs.dmg --pristine $F/dec/rootfs.dmg --mbr $F/mbr.bin \
    --out $F/userland-app --lockdown none --no-usb-net --no-web-proxy --no-ca-ogl --appsync --gles
python3 -c "import sys; sys.path.insert(0, 'imgtools'); import ipad1_rootfs as r, os
d = '$F/userland-app/pristine'
with r.Mounted(d + '/system.img', d + '/mnt-system') as m:
    r.activation_hook('$HOOK', os.path.join(m.mnt, r.LOCKDOWND))"    # HOOK: offline-activation-8C148/patch_lockdownd.py
# ipad1_nand.py build ... --out $F/userland-app/nand; dev3 = a copy + erased NOR; ipad1_keybag.py --board n18
cc -o build/ipad1-tools/sbicons tests/ipad1/sbicons.c $(pkg-config --cflags --libs libimobiledevice-1.0)
tests/ipad1/app-install.py --machine n18 --device $F/dev3 --kboot $F/kboot-nor-nov.bin --nor $F/dev3/nor.bin \
    --product-version 4.2.1 --ipa Harness.ipa --gl-tap 0.5,0.165 --out $F/runs/app
```

## Models: reused, varied, new

Classes as in LightTouchMac `docs/fidelity-ledger.md`: R register-level, H high-level emulation of what
the block does, P a documented quirk/patch, S stub. Addresses: the N18AP 8C148 DT (arm-io maps child
offsets at 0x80000000).

| Component | Model | How | Class |
|---|---|---|---|
| CPU, RAM | cortex-a8, 256 MiB at 0x40000000 + the iPad's mirror above it | as the iPad | R |
| VICs | three `pl192` at 0xbf200000 (DT reg 0x30000), daisy-chained | as the iPad, one fewer | R |
| PMGR + timer | `s5l8920.pmgr` (new): timebase 0x200/0x204, two event timers (count 0x208+4n, state 0x220+4n; the AP's rtclock on 0, IRQ 6; the IOP firmware's on 1, IRQ 5, its literals bf100208/220/20c/224); every other register a plain store | new | R (timers) / S (clocks, gates, MCU window at 0xbfc00000 unmapped) |
| GPIO IC | `s5l8930.gpio`, `ports=46` `int-groups=7` `pin-int-enable`: the pin's config word masks its interrupt (bit 4), 0x800+4g is the pending word (W1C) | variant | R |
| IOP | `s5l8930.iop` with `fw-size-mask` (0x114 = (-size) & 0x3ffff000, the v1 encoding) and `s5l8930.iop-core` with `dram-base=0` (the kext hands the IOP phys - 0x40000000) | variant | R |
| H2FMI | `s5l8930.h2fmi` at 0x81200000/0x81300000, IRQs 0x1f/0x1e | as the iPad | R |
| CDMA + AES | `s5l8930.cdma`, `version=1` (no channel-enable block: every channel live), 28 channels from IRQ 0x2a | variant | R |
| SHA-1, PKE | `s5l8930.sha1`, `ipodtouch.pke` | as the iPad | R |
| UART0-1 | `exynos4210.uart`, IRQs counting down from 0x18 | as the iPad | R |
| I2C0, I2C2 | `s5l8930.i2c`, IRQs 0x13/0x11 | as the iPad | R |
| PMU D1755 | the S5L8720's Dialog model (`pcf50633`): `event-count=4` (events 0x01-0x04, status 0x05, masks 0x09-0x0c), `shutdown-reg=0x0d`, `adc-reg=0x30`, `rtc-reg=0x4c`; IRQ to GPIO 0x9d | variant | H |
| Codec, Mikey, accelerometer | `cs42l58` 0x4a, `cd3272mikey` 0x3a, `lis302dl` i2c2 0x1d | shared | H |
| SPI0-1 | `ipodtouch.spi`; SPI1 with `tx-fifo-depth=65536` (the CDMA pushes the N1 firmware's 53196 bytes in one chain; the default 50000 dropped its tail) | variant | R |
| Multi-touch | the Zephyr model, `mt_profile_n81` (the N81's N1F55 sensor), ATN on GPIO 0xb4; AppleMultitouchN1SPI downloads the N1F55 firmware from `Common.mtprops` over HBPP and reads frames with 0xEB; its 0xEE result-length/result-data reads are answered (`mt_n1_read`) | variant | H |
| CLCD | the S5L8720's `ipodtouch.lcd` (M2 CLCD) at 0x85400000, IRQ 0x25; `fb-base` (iBoot's register state for kboot: window 1 320x480 32 bpp at the vram), scanout through dart0 (`ipod_lcd_set_iommu`; a frame not contiguous in PA is gathered page by page) | variant | R |
| DART | `s5l8920.dart` (new, AppleH2PDART): 64 MiB IOVA window at 0x3c000000, 16 x 4 MiB segments; STE port +0x08, config +0x0c (bit 31 enable); entries hold DRAM offsets (& 0x0ffff000) | new | R |
| NOR (grafted) | `ipodtouch.nor` on spi0, CS GPIO 0x1204, with `nor-rw` | shared | R (P: the DT node) |
| MIPI-DSIM, SWI, scaler | the iPod's at 0x89000000, 0x89100000, 0x85500000 | shared | R / H |
| USB | the S5L8720's PHY + DWC OTG (device mode), built-in host | shared | R |
| TV-out | the S5L8720's `ipodtouch.tvout`: SDO 0x85600000, mixers 0x85200000/0x85100000, IRQs 0x23/0x27 | shared | H |
| I2S0 | the A4's `s5l8930.i2s`, registers at 0x84500400, its TX FIFO window (MMIO 1) at 0x84500000 where CDMA channel 0x15 writes; the CDMA's `paced-base` puts that FIFO on the audio clock | variant | R |
| Everything else | the unimplemented window 0x80000000-0xbfffffff (SDIO/Wi-Fi, JPEG, VXD, AMC, PWM) | | S |

## Debts

1. **No NAND boot partition; a NOR instead (P, kboot)**. These boards boot from NAND: with DT
   `boot-from-nand` IOFlashPartitionScheme claims the flash for a boot-block partition table (magic
   `ndrG`: LLB, iBoot, NVRAM, effaceable) that only a restore writes (restored's `partition_nand_device`),
   and without one no FTL attaches. kboot renames the property, so the FTL takes the whole device from
   block 1 as on the iPad, and `--nor` grafts the N88's spi0/nor-flash node (nvram, effaceable) for a NOR
   the N18 does not have. Fix: the boot partition table (restored writes it, or the image builder does).
2. **Clock table (kboot)**: `clock-frequencies` is the iPad's cut to these DTs' 32 slots.
3. **ChipID fuses**: the K48's words.
4. **D1755 backlight**: undecoded; the panel is held lit (`backlight-enable-reg` points at a scratch byte).
5. **AMC**: not wired.
6. **Buttons**: GPIO only. On USB power (always, as on the iPad), Hold locks the panel and Hold or Home wakes it
   (2026-10-05, FirmwareKit device). The AP never deep-sleeps there. Unplugged (`usb-attached=off`), the locked
   device deep-sleeps about 2 minutes after boot: "System Sleep", then the D1755 driver's "pmu go hib" (0x805cc8f6 on
   8C148), which writes 0x6f = 0x80 and sets 0x26 in 0x0d. Bit 0 stays clear, so it is not power-off. Nothing models
   the PMU cutting AP power or a press powering it back, so it stays asleep. On hardware, LLB resumes the
   kernel. Findings so far:
   - The wake buttons are the PMU's STAT function (DT buttons: function-wake_button_menu/_hold, args 0x180/0x181).
   - The hib path first configures wake sources (0x805cb4c0, reads 0x50..0x57).
   - No resume address appears in PMGR, pram (top 16 KiB of DRAM) or SRAM after sleep.
   - Re-entering the kboot entry with DRAM kept and devices reset (an experiment) ends in the abort handler.
     So the kernel expects a separate resume entry, which LLB knows about. Next: find where xnu-1504's ARM sleep
     path leaves it (PMU scratch over I2C is the remaining candidate), then make kboot's reset loader branch there
     on a PMU wake.
7. **it_keybag**: the iPad's armv7 build (`build/ipad1-guest/it_keybag`), copied; same volume layout.
8. **GL scene on the panel**: the harness's GLES view renders into its 240x360 IOSurface (stride 960) and
   reads back right, but the panel shows that surface laid out linearly at the panel's 320-pixel stride
   (the top 270 rows, cyan/magenta stripes). The CLCD keeps scanning its own framebuffer (window 1 unchanged)
   and the scaler runs no transfer during the scene, so the copy happens in the guest's composite of the app
   surface. With CoreAnimation compositing through the GL bridge (FirmwareKit's `ca_ogl`), the same surface is
   read at 320x480 twice: two stacked copies, a black row at 270, and 88% fixture colour. The gate's
   fixture-colour test passes either way. Next: find which compositor reads the
   surface at panel geometry (SpringBoard's software CA with `CA_ENABLE_OGL=0`, or IOMFB's swap of the
   app's layer).
9. **No USB host port**: the OTG runs in device mode only, so regress's `boot`/`net` checks, which type on a
   `usb-kbd` on `usb-bus.0`, cannot run (Bus 'usb-bus.0' not found). `wifi`, `persist` and app-install do.

## iPhone 3GS (N88AP, S5L8920): `-M n88`

The same machine with the N88's board data: board-id 0, five UARTs (the bq27540 HDQ gauge on uart4, as the
iPad's), its own SPI NOR (no graft), the spi2 baseband controller with nothing on it, i2c0 accelerometer,
AK8973 compass, CS42L61 (the CS42L58 register model, as on the iPad) and CD3272. The baseband DT node is
unmatched by kboot (fill_dt) and carries GSMA's test IMEI 004999010640000 and the serial `TESTSNUM0000`.

State: docs/n88/README.md (4.2.1 home screen, power-off and persistence, app install).
