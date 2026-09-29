# iPod touch 1G (n45ap, S5L8900): `-M iPod-Touch-1G`

Milestone 0: devos50's `ipod_touch_1g` machine brought onto this QEMU 10 tree, sharing the iPod 2G's
models wherever the two SoCs agree and carrying the S5L8900-only blocks under `hw/arm/s5l8900_*.c`.
iPhone OS 1.1 (3A101a) boots through the real bootrom and iBoot-204 to the SpringBoard home screen
with working touch; lockdownd reports the device activated.

## What runs

- Bootrom jump table (verify/decrypt stubs at LLB_BASE) -> iBoot-204 (NOR, IMG2/8900 signatures via the
  hardware SHA1 and AES blocks) -> device tree -> 3A101a kernel -> every IOKit driver starts -> HFS root
  on the FMC NAND -> launchd, lockdownd (`[Activated]`), SpringBoard.
- Screen: iBoot's logo through CLCD window 2, the kernel's UI through window 1; screenshots via QMP
  `screendump` as the tests take them.
- Touch: the Zephyr2 digitizer over SPI2 with the 1G's ATN line (SYSIC GPIO group 4 bit 27); a QMP tap
  opens Settings.
- NAND: programs, multi-bank programs and block erases land in `nand-overlay=` with NAND semantics
  (erased pages read all ones, a program only clears bits, an erased page reads back as clean). On a
  store with real FTL spares a Settings change survives a clean power-off (the PMU shutdown path) and
  the next boot; after a hard quit the FTL's own restore rebuilds its tables (see debts).
- USB: the wrangler takes its PHY and publishes its host and device nubs; the device idles into sleep
  (`pmu go hib`) without a panic. Waking is not modelled: the kernel parks in `ml_arm_sleep` with
  interrupts masked for the PMU to cut the AP, and the resume path (bootrom/LLB back into the kernel)
  is skipped by the direct iBoot boot (debt 9).

Verified 2026-09-29 (commits on `ipod-1g`): home screen, Settings after a tap, no `unexpected CLCD
interrupt`, no panic, on devos50's public `n45ap_v1` assets.

## How to boot

Assets (never committed, never in this tree): devos50's `n45ap_v1` set under
`~/Developer/qemu-ios-files/ipod1g/`: `bootrom_s5l8900`, `iboot_204_n45ap.bin`, `nor_n45ap.bin`,
`nand/bank<0-7>/<page>.page`.

```
F=~/Developer/qemu-ios-files/ipod1g; cp $F/nor_n45ap.bin /tmp/nor.bin; mkdir -p /tmp/ovl
build/qemu-system-arm -M iPod-Touch-1G,bootrom=$F/bootrom_s5l8900,iboot=$F/iboot_204_n45ap.bin,nand=$F/nand,nand-overlay=/tmp/ovl \
  -drive if=pflash,format=raw,file=/tmp/nor.bin \
  -serial file:/tmp/serial.log -display none -audio driver=none \
  -qmp unix:/tmp/qmp.sock,server,nowait
```

The CPU is the default arm1176 (`-cpu max` faults in iBoot). NOR is written by iBoot/the kernel, so
copy it. SpringBoard configures at about 60 s of guest time; `screendump` then. Keys: Cmd+Shift+H home,
Cmd+L power (headless: QMP `input-send-event` for taps, as `tests/ipod/regress.py` does).

Machine properties: `bootrom`, `iboot`, `nand`, `nand-overlay`, `usb-wrangler-quirk` (bool, default
on: the phyRegistered guard, see debts), `tvout-workaround=<paddr>` (default off).

Traces: `LCD_TRACE`, `MT_TRACE`, `IT_FMC_TRACE`, `IT_TIMER_TRACE`, `IT_CLOCK_TRACE`.

## Models: reused, varied, ported, new

Classes as in LightTouchMac `docs/fidelity-ledger.md`: R register-level, H high-level emulation of what
the block does, P a documented quirk/patch, S stub.

| Component | Model | How | Class |
|---|---|---|---|
| CPU, VICs, RAM, SRAM | arm1176, two `pl192`, 128 MiB at 0x08000000 | shared, as-is | R |
| Bootrom + LLB stubs | vrom + jump-table stubs at LLB_BASE (+0x80 verify, +0x100 decrypt) | ported from devos50 | H |
| 8900 image engine | `8900` v1.0 format-3 payload decrypt (AES-128-CBC key 0x837) behind the decrypt stub | ported | H |
| NOR | `pflash_cfi02` on `-drive if=pflash` | shared QEMU model | R |
| Timer | `ipodtouch.timer`, `irqlatch=0xF8`, `freq-hz=12000000`; timers 0-3 unmodelled (logged) | variant by property | R (timer 4) / S (0-3) |
| Clock | `ipodtouch.clock`, `s5l8900=on` reset presets | variant | H |
| SYSIC / power controller | `ipodtouch.sysic`, `direct-boot`, `epoch=2`, `s5l8900` mask semantics (+0xC down, +0x10 up), 7 GPIO groups | variant | R |
| GPIO | `ipodtouch.gpio`, 0x20 pads | variant | R |
| Chip ID | `ipodtouch.chipid`, `word1/word2` | variant | R |
| UART x5 | `exynos4210.uart`, `s5l8720-irq`; optional `tx-char-ns` pacing (off) | shared | R |
| SPI0-2 | `ipodtouch.spi`, `s5l8900=on` (no TXCNT, write-to-clear TXEMPTY/RXREADY, 4-bit FIFO counts), `peripheral=` | variant (`set_spi_base` gone on all boards) | R |
| LCD panel (SPI1) | `s5l8900.lcdpanel`: ID bytes only | new | S |
| Multitouch (SPI2) | shared Zephyr2 model, ATN group/bit from the board | variant | H |
| CLCD | `ipodtouch.lcd`, `s5l8900=on`: window 1 at 0x58, window 2 at 0x70, +0x14 enable, +0x18 status/ack | variant | R (windows, irq) / H (blend, palette, VIDCON stored only) |
| AES | `ipodtouch.aes`, `addr-offset=0x80000000`, `s5l8900-compat` (devos50's UID/key-schedule convention) | variant | H |
| SHA1 | `ipodtouch.sha1`, hardware buffer readable | shared | R |
| FMC NAND + ECC + ADM | `s5l8900.fmc` (base + overlay page store, erase markers, program = AND), `s5l8900.nand-ecc` (stub), `s5l8900.adm` (the ADM firmware's command interface: 0x200/0x300 read, 0x400 multi-bank program, 0x500 program, 0x600 erase; FTL metadata in data3; result mailbox +0x30 with clean-page status) | ported, command set completed | R (store) / S (ECC) / H (ADM firmware) |
| USB OTG + PHY | `synopsys` OTG with the 8900 hwcfg, `ipodtouch.usbphys` | shared | R (wrangler, PHY, nubs up; no host attached yet) |
| DMA | two `pl080` | shared QEMU model | R |
| I2C0 lis302dl, I2C1 pcf50633 | shared; PMU `shutdown-reg=0x0c` (1.x: 0x0a is the fourth IRQ mask) | variant | H |
| SDIO | `ipodtouch.sdio` | shared | H |
| TVOut (mixer1/2, sdo) | `ipodtouch.tvout` | shared | S |
| MBX (GPU) | id stub: 0x12c, 0xf00, 0x1020 | new | S |
| Watchdog, I2S0-2, MPVD, H264 | RAM-backed windows | new | S |
| Edge IC | shared | shared | H |

## Debts (each also a row in LightTouchMac `docs/smoke.md`)

1. **USB wrangler ordering (P, `usb-wrangler-quirk`)**. AppleS5L8900XUSBWrangler::start calls
   `addNotification` for the PHY and stores the notifier at `this+0x6c` afterwards; `addNotification`
   calls phyRegistered synchronously for a PHY already published, and phyRegistered calls `remove()`
   through the NULL field. Nothing in hardware decides the order: between the two starts neither driver
   waits on an OTG or PHY register (the PHY's start is `IODelay(200 us)` plus clock/power-gate calls;
   the wrangler's only wait is its interrupt source, i.e. the VIC, which waits for AppleARMCPU's
   initCPU, which waits for the power controller's `function-cpu_idle`, which waits for the clock
   controller's matched state). The PHY needs one of those links, the wrangler four, and the IOKit
   config-thread pool runs the PHY's job first; the order is identical at `-icount shift=1/3/7` and with
   `io=0`. So there is no latency to model. The old quirk hid the PHY, which left the wrangler without
   one: its `setPowerState(0)` (sleep and shutdown) dereferenced it. The quirk is now a guard in
   phyRegistered (`cmp r0,#0; ldrne r3,[r0]; blne <ldr pc,[r3,#0x54]>`), found by its instruction
   words once the kernelcache is in RAM, so the wrangler takes its PHY as the asynchronous path would.
   P-class guest patch; the real fix is whatever makes real IOKit order these, which is not hardware.
2. **NAND store (R) and the stores it runs on**. The FMC/ADM now implement the FTL's whole command set:
   the multi-bank program 0x400 (the FTL's context flush; each page streams 2048 bytes then a 16-byte
   pad descriptor, per the driver's DMA list), block erase 0x600 (overlay pages dropped, a
   `blk<N>.erased` marker hides the base block), erased pages read all ones and report clean (0xFE in
   the ADM result mailbox, which the driver turns into the FTL's "found clean page"), and a program
   ANDs into the page, logging any program of a page that is not erased. devos50's public image cannot
   be consistent with that: its FTL context says the next context page is page 0 of the context block,
   which holds the map tables, and its filesystem sits in the FTL's free pool, so the first flush
   programs 440 non-erased pages and lockdownd gets EIO. On a store with real spares and the FTL's own
   blocks kept free (FirmwareKit N45NAND's layout, plus a VFL context the VFL accepts) a Settings change
   survives a clean power-off and the next boot, and after a hard quit the FTL's restore succeeds; the
   3A101a root volume is unjournaled HFS+ with no fsck at boot, so what a hard quit leaves is a
   filesystem-level question (see LightTouchMac smoke #12).
3. **AES compatibility convention (H)**. `s5l8900-compat` reproduces devos50's UID key convention
   (decrypt schedule, direction from the second KEYLEN write) because the public image set was built
   against it; the real UID is unknown. Real-device keys would replace this.
4. **CLCD VIDCON/blend registers (H)**: stored and echoed, not interpreted; the palette and window-2
   alpha are ignored. Window 2 shows only until window 1 has a base.
5. **RAM-backed windows (S)**: watchdog, I2S0-2, MPVD, H264 accept and return whatever is written;
   audio/video paths are not modelled.
6. **TVOut workaround property**: devos50's per-build zero-word overlay is `tvout-workaround=<paddr>`,
   off by default; 3A101a on these assets does not need it.
7. **Timers 0-3 (S)**: touched by the kernel, logged, never fire.
9. **Deep sleep and resume (S)**. `pmu go hib` ends in `ml_arm_sleep` (a `b .` with IRQ/FIQ masked,
   0xc005a6d0 in 3A101a): on silicon the PMU removes AP power and a button brings it back through the
   bootrom and LLB into the kernel's resume path. None of that exists here (no LLB, no power-cut
   model), and the buttons drive only their GPIO pads, not the PMU wake source the DT names
   (`button-wake`: PMU interrupt 0x0a, `'STAT'` 0x100).
8. **Panel/backlight (S)**: `s5l8900.lcdpanel` answers ID reads only; brightness comes from the PMU.

## Files

`hw/arm/ipod_touch_1g.c`, `include/hw/arm/ipod_touch_1g.h`, `hw/arm/s5l8900_{fmc,nand_ecc,adm,lcd_panel}.c`
and headers, `hw/arm/Kconfig` (`IPOD_TOUCH_1G` selects `IPOD_TOUCH_2G`), `hw/arm/meson.build`,
`configs/devices/arm-softmmu/default.mak`. Shared-model property additions are in the models named above.
