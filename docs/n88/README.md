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
| 4.2.1 8C148a | activated home screen ("No Service") | yes | yes | yes (Harness) | no: the machine has no QEMU_CALL (GL bridge) yet |

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
- Apps: with an `--appsync` system image, `ideviceinstaller install Harness.ipa` completes, installd lists
  `com.qemuios.harness`, and its icon (page 2) launches it (`runs/app-harness-8C148a.png`).
- Status bar: "No Service". The baseband (spi2, IFX v1) is the cellular stream's.

## How to boot

Assets under `~/Developer/qemu-ios-files/n88/` (never committed): the 8C148a IPSW, its keys page, the
`ipad1_fw.py` output in `dec/`, `identity.json`, `mbr.bin` (as on the N18).

```
F=~/Developer/qemu-ios-files/n88
imgtools/ipad1_rootfs.py build --rootfs $F/dec/rootfs.dmg --pristine $F/dec/rootfs.dmg --mbr $F/mbr.bin \
    --out $F/userland-app --lockdown none --stash none --no-usb-net --no-web-proxy --no-ca-ogl --appsync
imgtools/ipad1_rootfs.py bake $F/userland-app/pristine \
    --activation-hook ~/Developer/qemu-ios-files/ipad1/offline-activation-8C148/patch_lockdownd.py
imgtools/ipad1_nand.py build --no-whitening --epoch 3 --geometry k48-16g --mbr $F/mbr.bin \
    --kernelcache $F/dec/kernelcache.mach --system $F/userland-app/pristine/system.img \
    --data $F/userland-app/pristine/data.img --out $F/userland-app/nand
imgtools/s5l8920_kboot.py n88 --identity $F/identity.json --nor $F/dec $F/kboot-nor.bin \
    "serial=3 debug=0x8 -v amfi_allow_any_signature=1 cs_enforcement_disable=1"
mkdir $F/dev3; cp -cR $F/userland-app/nand $F/dev3/nand
python3 -c "open('$F/dev3/nor.bin','wb').write(b'\xff'*0x100000)"
imgtools/ipad1_keybag.py $F/dev3/nand $F/dev3/nor.bin --dec $F/dec --ramdisk 038-0082-001-ramdisk.dmg \
    --identity $F/identity.json --board n88
build/qemu-system-arm -M n88,kboot=$F/kboot-nor.bin,nand=$F/dev3/nand,nand-overlay=OV,nor-rw=NORCOPY \
    -display none -serial file:serial.log -qmp unix:/tmp/n88.qmp,server,nowait
```

`bake` needs `contrib/ipad1-guest/build.sh` and `contrib/guest-package/build.sh` first, `--appsync`
`contrib/appsync/build.sh`. The writable NOR holds effaceable and NVRAM: give each run its own copy. The
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

`explicit-start`: the s5l8920x firmware leaves FMI control at 3 or 5 between transfers and starts each
with its own control write. The last page of a multi-page read follows a status poll, not a read command,
and a write's FIFOs fill while control is still 5 from the previous page, before the next CE is selected.
With the property a control 3 takes any CE whose latched page no transfer has taken, and only a control-5
write arms a write transfer. Without it, every 8-page READ_MULTIPLE timed out (2 s, status 0x8000001c, the
last page's meta never sent) and single-page writes were captured against the stale CE and dropped
("Failed Index read" panic). `IOP_RING_TRACE=1` compares every READ/WRITE_MULTIPLE page with the store.

## Gates run (2026-10-05, branch n88)

N88 4.2.1: boot to home screen, unlock, tap, Hold sheet, power-off, persist (afcclient marker), IPA install
and launch: PASS. Shared-model checks on this build: iPad 7B500 `tests/ipad1/regress.py --checks
boot,persist,gles` PASS; iPod 2G `tests/ipod/regress.py --checks boot` PASS; N81 `regress.py --machine
iPod-Touch-4G --checks boot` PASS; N18 dev2 unlock to the home screen with touch PASS.

## Debts

1. **No GL bridge, agent or guest package**: `s5l8920.c` has no QEMU_CALL coprocessor hook (ipad1.c's
   `ipad1_qemu_call`), so `--no-ca-ogl` (software CoreAnimation) and GL apps cannot draw; `it_boot` from
   `bake` traps.
2. **Baseband**: spi2 bare; "No Service" (cellular stream).
3. **Bluetooth**: BTServer respawns (no BCM4325 on uart3) unless the image disables it (`bake` does).
4. **Camera**: AppleH2CamIn times out on its ISP mailbox (no ISP model); mediaserverd survives it.
5. As the N18: kboot with the NOR (no NAND boot blocks), K48 fuses and clock table, D1755 backlight and
   wake latch, Wi-Fi.
6. Other majors (3.1.x, 5.x, 6.x) not tried.
