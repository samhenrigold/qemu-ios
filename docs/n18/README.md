# iPod touch 3G (N18AP, S5L8922): `-M n18`

The S5L8920/8922 machine (`hw/arm/s5l8920.c`): the A4's blocks (`s5l8930_*`) at this SoC's addresses and
interrupt numbers, the S5L8720's display, USB and Dialog PMU models, and two blocks of its own (PMGR,
`s5l8920_pmgr.c`). iOS 4.2.1 (8C148) boots by kboot (no iBoot) from a NAND store to launchd. The iPhone 3GS
(N88AP, S5L8920) is the next board on the same machine file.

## What runs (2026-10-04)

- kboot bundle (`imgtools/s5l8920_kboot.py n18`, ipad1_kboot's iBoot stand-in with the board's values) ->
  xnu-1504.58.28 RELEASE_ARM_S5L8922X -> every platform driver starts: VICs, GPIO IC, performance
  controller, IOP (the kernel's EmbeddedIOP firmware on the second core), NAND through the IOP and the
  H2FMI, SPI, I2C, D1755 PMU, CS42L58, M2 CLCD, MIPI-DSIM, SWI, scaler, USB PHY/OTG (device mode,
  "Connected to a USB host"), Zephyr multi-touch on SPI1 ("successfully started").
- Root: YaFTL mounts the store's MBR volumes ("Creating block device of 3925449 sectors"), BSD root
  disk0s1, fsck clean, /private/var on disk0s2, launchd starts its jobs.
- Stops at `FATAL KEYBAG ERROR: kb_load` / `REBOOTING INTO RESTORE MODE`: 4.x needs formatted effaceable
  storage and a system keybag (the iPad's 8C148 restore-ramdisk one-shot, `docs/ipad1/ios4.md`), and this
  board keeps effaceable storage on NAND behind the boot partition table (debt 1). The reboot then panics
  in the IOP ("timed out with partially completed command"): unexamined.
- The restore ramdisk boots as md0 too (`--ramdisk`): "BSD root: md0".

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

Machine properties: `kboot`, `nand`, `nand-overlay`.

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
| SPI0-1 | `ipodtouch.spi`; SPI1 the Zephyr multi-touch (iPod profile), ATN on GPIO 0xb4 | shared | R / H |
| CLCD | the S5L8720's `ipodtouch.lcd` (M2 CLCD) at 0x85400000, IRQ 0x25 | shared | R |
| MIPI-DSIM, SWI, scaler | the iPod's at 0x89000000, 0x89100000, 0x85500000 | shared | R / H |
| USB | the S5L8720's PHY + DWC OTG (device mode), built-in host | shared | R |
| Everything else | the unimplemented window 0x80000000-0xbfffffff (TV-out, SDIO, DARTs, JPEG, VXD, audio) | | S |

## Debts

1. **No NAND boot partition (P, kboot)**. These boards boot from NAND: with DT `boot-from-nand`
   IOFlashPartitionScheme claims the flash for a boot-block partition table (magic `ndrG`: LLB, iBoot,
   NVRAM, effaceable) that only a restore writes (restored's `partition_nand_device`), and without one no
   FTL attaches. kboot renames the property, so the FTL takes the whole device from block 1 as on the
   iPad, and there is no NVRAM and no effaceable storage. Fix: the boot partition table (restored writes
   it, or the image builder does), or the NOR graft the A4 boards use.
2. **Clock table (kboot)**: `clock-frequencies` is the iPad's cut to these DTs' 32 slots.
3. **ChipID fuses**: the K48's words.
4. **D1755 backlight**: undecoded; the panel is held lit (`backlight-enable-reg` points at a scratch byte).
5. **Wi-Fi (SDIO), audio (I2S at 0x84500000, AMC), TV-out**: not wired.
