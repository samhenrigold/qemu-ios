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
   - The gyro (ap3gdl/mpu3100 at 0x68). Its driver frees itself.
   - The ALS/prox ct700 (i2c0 0x29). It is probed for and skipped.
   - The Highland Park audio processor (i2c0 0x3e, uart6, i2s2).
   - The GPS (bcm4750 on uart4).
   - The cameras and ISP (AppleH3CamIn mailbox timeouts, as on N81).
3. **Compass**: the AK8973 stands in for the AK8975B pair.
4. ~~Accelerometer mounting~~ (2026-10-04): `accel_mount` "-2,1,-3" (the DT orientation's transpose).
   Safari turns with `accel-orientation` 1/3/4 as on hardware.
5. All of N81's debts apply: no iBoot or NAND boot, K48's 16 GB NAND geometry, K48's panel ID, uncalibrated
   touch edges.
