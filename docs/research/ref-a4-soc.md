> Status: research, superseded by `hw/arm/ipad1.c`, `hw/arm/s5l8930_*.c` and `../ipad1/addresses-7B500.md`.

## S5L8930 (A4) SoC map from openiBoot `plat-a4` — cross-checked with iEmu `hw/s5l8930.h`

Source roots: `OIB` = `/home/user/iDroid-Project/openiBoot`, `IEMU` = `/home/user/teknogeek/iemu/hw` (danzatt/QEMU-s5l89xx-port `hw/s5l8930.{c,h}` are byte-identical to iEmu: `diff -q` empty). Build config: `OIB/plat-a4/SConscript:5` defines `ARM_A8, CONFIG_A4`; `OIB/plat-a4/iPad1G.SConscript:15` defines `CONFIG_IPAD_1G`, `MACH_ID=3593`, and adds module `nor-spi`; platform is built with `MALLOC_NO_WDT` (no watchdog driver in plat-a4).

### 1. Memory map / peripheral table

#### 1.1 Memory regions
| Region | Address | Source |
|---|---|---|
| VROM / exception vectors | 0x00000000 (`MemoryStart`, `ExceptionVector`, `OpenIBootLoad`) | `OIB/plat-a4/includes/hardware/a4.h:8,18,23` |
| DRAM | 0x40000000–0x50000000 (256 MiB; iPhone4 to 0x60000000) | `a4.h:10-15` |
| LargeMemoryStart / HeapStart | 0x46000000 / 0x48000000 | `a4.h:16,25` |
| Page table | RAMEnd-0x8000 = 0x4FFF8000 | `a4.h:26` |
| Framebuffer (iPad) | 0x4F700000 (defined; openiBoot actually mallocs its window) | `OIB/plat-a4/includes/hardware/clcd.h:17`, `clcd.c:721` |
| "AMC0" (mapped cacheable) | 0x84000000–0x84400000; `AMC0Higher` 0x84C00000–0x85000000 mapped to 0x84000000 | `a4.h:34-37`, `OIB/plat-a4/mmu.c:47,53` |
| RAM alias 0xC0000000 | openiBoot maps 0xC0000000–0xFFFFFFFF → 0x40000000 **via MMU** (`mmu.c:56`); iEmu maps it as a hardware RAM alias `RAM_HIGH_ADDR 0xC0000000` (`IEMU/ipad1g.h:11`, `ipad1g.c:427`) | |
| PeripheralPort | 0x38000000 (leftover S5L8720 value, no A4 use) | `a4.h:33` |
| iEmu: LLB load 0x84000000, iBoot load 0x5FF00000, 1 MiB RAM at 0 ("iopram") | `IEMU/ipad1g.h:6-7`, `ipad1g.c:431-432` |
| DMA alignment | 0x40 | `a4.h:57` |

**NOT FOUND**: SRAM base/size for S5L8930 (openiBoot never references an SRAM; iEmu treats 0x84000000 as the LLB load region, which openiBoot maps cacheable as "AMC0"). No CPU-ID/MIDR value in either tree.

#### 1.2 Interrupt controllers — 4 × PL192
| Item | Value | Source |
|---|---|---|
| VIC0..3 | 0xBF200000, 0xBF210000, 0xBF220000, 0xBF230000 (stride 0x10000) | `OIB/plat-a4/includes/hardware/interrupt.h:13-16`; `IEMU/s5l8930.h:20-23` |
| IRQ numbering | 0..127 (`VIC_MaxInterrupt 0x80`), 32 per VIC, `vic = irq>>5` | `interrupt.h:8-9`, `interrupt.c:101-102` |
| Registers used | IRQSTATUS 0, RAWINTR 8, INTSELECT 0xC, INTENABLE 0x10, INTENCLEAR 0x14, SWPRIORITYMASK 0x24, VECTADDRS 0x100+4i, ADDRESS 0xF00, PERIPHID 0xFE0-0xFEC | `interrupt.h:20-31` |
| Init | clear enables on all 4, INTSELECT=0 (all IRQ), SWPRIORITYMASK=0xFFFF, VECTADDRi = irq number | `interrupt.c:8-35` |
| Chaining | iEmu creates 4 pl192 and daisy-chains them with `pl192_chain()` (only VIC0 wired to CPU IRQ/FIQ) | `IEMU/s5l8930.c:1391-1406` |
| Edge type | openiBoot A4 returns "edge not supported" (no EDGEIC on A4, unlike S5L8720 `plat-s5l8720/interrupt.c:20-21`) | `interrupt.c:113-114` |

Note: plat-a4 does **not** check VIC PERIPHID (S5L8720 does, expects 0x192 — `plat-s5l8720/interrupt.c:9-17`); a real iBoot may.

#### 1.3 IRQ number table (main VIC)
| IRQ | Device | Source |
|---|---|---|
| 0x03 | IOP mailbox (iEmu) | `IEMU/s5l8930.h:93` |
| 0x05 | Timer (second/IOP timer, count reg 0xBF10200C) | `timer.h:42-44`, `IEMU/s5l8930.h:30` |
| 0x06 | Event timer (count reg 0xBF102008) | `timer.h:38-40` |
| 0x0D | USB OTG | `OIB/usb-synopsys/includes/hardware/usb.h:31`, `IEMU/s5l8930.h:51` |
| 0x13/0x14/0x15 | I2C0/1/2 | `OIB/plat-a4/i2c.c:12-14` (field 12), `IEMU/s5l8930.h:69-71` |
| 0x16..0x1C | UART0..6 (`UART_INTERRUPT + i`) | `uart.h:34`, `uart.c:185` |
| 0x1D..0x21 | SPI0..4 | `spi.h:17-21`, `spi.c:51-55` |
| 0x22/0x23 | H2FMI0/1 (iEmu routes these to the IOP's VIC) | `h2fmi.h:9-10`; `IEMU/s5l8930_iop.c:202-210` |
| 0x26 | SDIO | `sdio.h:6`, `sdio.c:605` |
| 0x29/0x2A/0x2B/0x2D | VOLUP/VOLDOWN/RINGER/HOLD (GPIO-derived) | `buttons.h:14-17` |
| 0x30 + n | CDMA channel n (n=0..31 → 0x30..0x4F) | `dma.h:13`, `cdma.c:120-122` |
| 0x33 | WDT (unverified, copied from 8720) | `a4.h:42` |
| 0x74 | GPIO IC | `gpio.h:8`, `gpio.c:118` |

Not found in openiBoot: SHA1, display pipe/CLCD/MIPI, AMC, IOP IRQ numbers (IOP=3 only in iEmu).

#### 1.4 Timers (base shared with PMGR)
| Item | Value | Source |
|---|---|---|
| TIMER base | 0xBF100000 (same as `PMGR0_BASE`); 7 timers at +0x0,0x20,0x40,0x60,0xA0,0xC0,0xE0; per-timer CONFIG 0, STATE 4, COUNT_BUFFER 8, COUNT_BUFFER2 0xC, PRESCALER 0x10, +0x14 | `timer.h:8-24,36` |
| 64-bit free-running counter | TICKSLOW 0xBF102000, TICKSHIGH 0xBF102004 | `timer.h:25-26`, `timer.c:231-233` |
| Event timer | count 0xBF102008, state 0xBF102010 (STATE_START 1, MANUALUPDATE 2), IRQ 6; alt: count 0xBF10200C, state 0xBF102014, IRQ 5 | `timer.h:39-48`, `event.c:22-24,63-64` |
| Clock | `TicksPerSec = CalculatedFrequencyTable[0] = 24000000` (24 MHz) | `clock.c:382,422`; iEmu `ticks = ns/1000*24` `IEMU/s5l8930.c:123` |
| Misc regs | UNKREG0 0x88 (bits 0-3 enable), 0x8C-0x98, IRQSTAT 0x10000, IRQLATCH 0xF8 | `timer.h:27-33` |
| `timer_setup()` writes | 0xBF500004 \|= 0x80000000; 0xBF101218=0xEF0000, 0xBF10121C=0xFC020408, 0xBF101220=0x27C0011, 0xBF101228=0x1830006, 0xBF101230=0x12F0006, 0xBF101224=0x20404, 0xBF10122C=0x20404, 0xBF101234=0x20409, 0xBF101200=0, 0xBF101000=0x3F | `timer.c:46-58` |
| iEmu model | maps 0xBF102000 size 0xFFFF; +0 low, +4 high, +8 count, +0xC count2, +0x10 state, +0x14 state2, +0x3030 scratch, +0x4000 (=0xBF106000 POWER_ID) returns 0x2020001 | `IEMU/s5l8930.h:29-38`, `s5l8930.c:121-143` |
| Clock gate | 0x25 (defined, not switched by plat-a4) | `timer.h:37` |

#### 1.5 PMGR / clock controller (0xBF100000)
| Offset | Meaning | Source |
|---|---|---|
| +0x00/04 APLL CON0/1, +0x08/0C MPLL, +0x10/14 EPLL, +0x20/24 VPLL | PLL control; CON0: bit31 ENABLE, bit29 LOCKED, bit27 UPDATE, M bits3-12, P bits14-19, S bits0-2; CON1: FSEL bits17+ (5/0xD/0x1C by freq), enable value 0x960 | `clock.h:13-45`, `clock.c:244-278` |
| PLL freq | `M * 48000000 / (P << S)`; iEmu reset values 0xA00187D1 (APLL=1000 MHz), 0x40000000 (MPLL off), 0xA0010559 (EPLL=1026 MHz), 0xA0008205 (VPLL=48 MHz), CON1 0x380960 | `clock.c:275-277`, `IEMU/s5l8930.c:388-398` |
| +0x30 CLOCK_CON0 (bits0-1 cleared at reset) | `clock.h:22`, `clock.c:241` |
| +0x40..+0xFC | 48 clock-config regs (`CLOCK_CON_BASE`, index i at +0x40+4i); bit28-29 = source select; dividers per `clock_dividers[]` (mask 0x1F/0x1F00/0x3E000/0x7C0000/0xF800000); +0x40 = ARMCLK (index 5, low 5 bits multiplier, `mul*4`) | `clock.h:50-58`, `clock.c:28-100,328,452` |
| Named regs | 0x44-0x54 PREDIV0-4, 0x70/74/78 BASE0/1/2, 0x68 MEDIUM0, 0xCC MEDIUM1, 0x60/64 NCO_REF0/1, 0xA0 CDMA, 0x98/0x90/0x9C HPERF0/1/2, 0x8C MPERF, 0xA4-0xB0 LPERF0-3, 0x94 VID0, 0x5C VID1, 0x84 AUDIO, 0x88 MIPI, 0xB8 SDIO, 0xBC CLK50, 0xDC-0xEC SPI0-4, 0xF0-0xF8 I2C0-2 | `clock.c:50-99` |
| Reset/init tables | `clock_reset_values[48]` / `clock_init_values[48]` (iPad path: `0xB0000000, 0x80000000, ...` at index 41) | `clock.c:102-134` |
| +0x1010 + 4·gate | Clock gates, 0..0x3F: bits0-3 request, bits4-7 actual state (poll until equal), bit31 = reset pulse | `clock.h:60-61`, `clock.c:141-155,187-198` |
| +0x5030 CLOCK_CON1 (write 7, wait bit16 clear) | `clock.h:23-25`, `clock.c:289-290` |
| +0x50C0 perf table (3 pairs: 0x8241 / 0x9009) | `clock.h:47-48`, `clock.c:280-287` |
| +0x6000 POWER_ID | bits31-24 = epoch (must equal chipid power epoch), bits 0-23 board-id/gpio strap word written by gpio_setup | `power.h:20-21`, `miu.c:9`, `gpio.c:83-115` |
| Default gates on | mask 0x80000080300002 → gates 1,20,21,23,55 | `clock.c:225` |
| Frequency table indices | 5 = CPU, 6 = memory, 27 = bus, 32 = peripheral, 33 = NAND, 0 = fixed 24 MHz | `clock.c:200-220` |

**Important**: `clock_setup()` has the PLL/reset path under `if(0)` (`clock.c:395-404`) — openiBoot only writes the 48 clock-config regs and reads PLLs; PLL programming is left to iBoot/LLB.

Clock gate indices used by plat-a4 (`clock_gate_switch` arg): 0xB HPERF2, 0xD, 0xF, 0x11 MIPI-DSIM, 0x12 DISPLAY_PIPE, 0x13 RGBOUT (`clcd.c:243-256`, `mipi_dsim.h:6`), 0x14 CDMA (`cdma.c:70`), 0x18 USB OTG, 0x1D USB PHY (`usb.h:29-30`, `usbphy.h:48`), 0x1B "EDRAM" (`a4.h:47`, only used on 1G PHY), 0x24 SDIO (`sdio.h:5`), 0x25 timer, 0x27/0x28 H2FMI0, 0x29/0x2A H2FMI1 (`h2fmi.h:7-8`, `h2fmi.c:753-754`), 0x2B-0x2F SPI0-4 (`spi.h:11-15`), 0x2C GPIO (`gpio.h:34`, conflicts with SPI1, unused), 0x30-0x36 UART0-6 (`uart.h:33`, `uart.c:137`), 0x39/0x3A/0x3B I2C0-2 (`i2c.c:12-14` last field; the `i2c.h:46-47` values 0x24/0x26 are stale), 0x3E backlight PWM (`clcd.c:781`). `clock_gate_reset` only pulses gates 22,36,39,41 (`clock.c:187-198`).

#### 1.6 GPIO (0xBFA00000, IRQ 0x74)
| Item | Value | Source |
|---|---|---|
| Pin numbering | `port<<8 \| pin`, pin 0..7, register index = 8·port+pin, `NUM_GPIO 276` (35 ports); one 32-bit reg per pin at `GPIO + idx*4` | `gpio.c:14,238,253,286` |
| Per-pin bits | bit0 = data; bits1-3 = int mode (0x4 level-hi,0x6 level-lo?,0x8/0xA edge,0xC autoflip) + 0x200 to enable; bits7-8 pull (01 down, 11 up); function values 0x210 input, 0x212 output-low, 0x213 output-high, 0x230/0x250/0x270 alt funcs, mask 0x27E/0x27F/0x3FF | `gpio.c:161-190,258-336` |
| Reset table (iPad variant, 276 entries) | `gpio.c:53-76` |
| GPIOIC | +0xC00 pending-block summary, +0x800 disable, +0x840 enable, +0x880 status (each +4·block, block = gpio>>5) | `gpio.h:17-20`, `gpio.c:128-217` |
| Board strap read | if POWER_ID bit0 clear: read pins 0x504,0x503,0x502 (→bits 18-16 with chipid GPIO epoch in bit 19) and 0x305,0x304,0x301,0x202 (→bits 11-8), OR 1, store to POWER_ID | `gpio.c:83-116` |
| iEmu | returns 1 for reads at +0x9C and +0xA0 (pins 0x407, 0x500, "board id?"), everything else 0, size 0x3FF | `IEMU/s5l8930.c:1205-1235` |
| Buttons | HOME 0x0, HOLD 0x1, VOLUP 0x2, VOLDOWN 0x3 (pin ids passed to `gpio_pin_state`) | `buttons.h:6-10` |
| iPad-specific pins | 0x1404 panel reset, 0x1403 backlight enable, NOR CS 0x505, SDIO reset 0x6 | `clcd.c:395,787`, `nor.h:50`, `sdio.h:7` |
| Old-style regs (unused on A4) | INTLEVEL 0x80, INTSTAT 0xA0, INTEN 0xC0, INTTYPE 0xE0, FSEL 0x320 | `gpio.h:11-15` |

#### 1.7 UARTs (Samsung S3C-style, 7 ports)
Base 0x82500000, UARTn at +n·0x100000 (`uart.h:7-16`; iEmu `S5L8930_UART0_BASE 0x82500000`, `s5l8930.h:16`). Registers ULCON 0, UCON 4, UFCON 8, UMCON 0xC, UTRSTAT 0x10, UERSTAT 0x14, UFSTAT 0x18, UMSTAT 0x1C, UTXH 0x20, URXH 0x24, UBAUD 0x28, UDIVSLOT 0x2C (`uart.h:18-29`) — **identical offsets and bit fields to plat-s5l8720** (`plat-s5l8720/includes/hardware/uart.h:14-25`). UART0 = console (no UMCON/UMSTAT, flow control off, IRQ-mode rx buffer 0x400) `uart.c:13-16,160,181`; UART1 = radio (`radio.h:4`); UART4 = display "MCU" at 250000 baud (`mcu.c:291`); UART5 gets ULCON=7 (`uart.c:151-153`). Clock source EXT_UCLK0 (bits 10-11 = 1), sample rate 16, baud div = 24 MHz/(baud·16)−1 (`uart.c:155-156,292-295`). IRQ mode sets UCON bit 12 (`uart.c:391,489,497`) and `UART_UCON_MODE_IRQ (5<<12)`; ISR acks UTRSTAT/UERSTAT by write-back (`uart.c:90-93`). iEmu reuses its s5l8900 UART model for UART0 (`IEMU/s5l8930.c:1427`).

#### 1.8 SPI0–4
| Item | Value | Source |
|---|---|---|
| Bases | 0x82000000, 0x82100000, 0x82200000, 0x82300000, 0x82400000 | `spi.h:5-9`, `IEMU/s5l8930.h:81-89` |
| Registers | 0x00 control (1=run, bits2/3 fifo reset), 0x04 config, 0x08 status (bit0 rx, bits6-10 tx-fifo-left, bits11-15 rx-fifo-left, 0x400002 tx-busy; write-1-clear), 0x0C pin (=2), 0x10 txData, 0x20 rxData, 0x30 clkDiv, 0x34 rxCount, 0x38-0x48 unknown, 0x4C txCount | `spi.c:12-30,50-56,87-109,195-221,289-356` |
| Config bits | bit0 no-tx-junk/rx-en, bit1 CPHA, bit2 CPOL, 0x18 master/ENSCK, 0x20 INT mode (0x40 DMA), 0x80/0x100 rx/tx enable, bit14 clock src NCLK(24 MHz), bits15-16 word size, 0x200000 tx-count-mode | `spi.h:26-35`, `spi.c:89-102,207-218,261-268` |
| IRQ/gates | 0x1D-0x21 / 0x2B-0x2F; only SPI0-2 enabled by openiBoot | `spi.c:51-55,365` |
| NOR | SPI0, CS GPIO 0x505, 12 MHz, 8-bit; JEDEC read | `OIB/nor-spi/includes/hardware/nor.h:50-51`, `nor.c:60,387,525-526` |
| Multitouch | **NOT FOUND** in plat-a4 (openiBoot only has multitouch for S5L8900) | grep of `OIB` |
| iEmu | same offsets 0x0-0x38, size 0x3C; SPI0 backs a `pflash_spi` NOR (AT25 id 0x1f/0x45/0x02, 4096-byte blocks, 256-byte pages) | `IEMU/s5l8930_spi.c:17-25,264-279` |

Layout is the **same as plat-s5l8720** (`plat-s5l8720/includes/hardware/spi.h:13-26`), except MAX_TX_BUFFER 0x1F vs 16 (`spi.h:42` vs 8720 `:41`).

#### 1.9 I2C0–2 (new A4 controller, NOT the S3C IICCON layout)
Bases 0x83200000 / 0x83300000 / 0x83400000 (`i2c.h:5-7`, `IEMU/s5l8930.h:66-68`); IRQ 0x13/0x14/0x15; gates 0x39/0x3A/0x3B; SCL/SDA GPIOs 0x904/0x903, 0x905/0x906, 0xA00/0x907 (`i2c.c:12-14`). Registers (`i2c.h:10-17`, `i2c.c:45-46,59-69,105-117`): +0x00 slave addr (7-bit, `addr>>1`), +0x08 init 0x30, +0x0C status (bit5=0x20 → NACK/error; write back to ack IRQ), +0x10 sub-register byte, +0x14 =0, +0x18 byte count (≤0x80), +0x20 data FIFO (write tx bytes / read rx bytes), +0x24 command: bit0 = write, bit2 = start. iEmu model: `IEMU/s5l8930_i2c.c:12-17,151-182` (same offsets, ACK/IRQ semantics hacked). I2C devices: PMU bus 0 addr 0xE8/0xE9 (7-bit 0x74; iEmu attaches `pcf50633` at 0x74 + "ipadchg" at 0x08 — `IEMU/s5l8930.h:76-78`), accelerometer LIS331DLH bus 2 addr 0x32/0x33 WHOAMI 0x32 (`accel.h:6-19`). S5L8720's I2C is Samsung IICCON/IICSTAT/IICADD/IICDS at 0/4/8/C (`plat-s5l8720/includes/hardware/i2c.h:9-17`) → **not reusable**.

#### 1.10 CDMA (0x87000000) and DMA-AES (0x87800000)
| Item | Value | Source |
|---|---|---|
| Global regs | +0x00 channel-start bitmask (per 32-ch group +4·(ch>>5)), +0x08 stop mask, +0x10 status bitmask, +0x18 error int | `dma.h:10-13`, `cdma.c:75-97` |
| Channel n regs (n<<12) | +0x00 ctrl/status (write 2 = abort; 0x1C0009 \| aesctx<<8 = start with IRQ; bits16-17 state 1=running; 0x40000 error, 0x80000 IRQ ack, 0x100000 spurious), +0x04 settings (bit1 dir out, bits2-3 word size 1/2/4, bits4-6 burst 1..32, bits16-21 peripheral id), +0x08 txrx (peripheral FIFO phys addr), +0x0C size, +0x14 segment-list phys ptr | `dma.h:16-18`, `cdma.c:150-203,302-310,410-441,540` |
| Descriptor (32 B) | `{next, flags, addr, len, iv[4]}`; flags: 3 = data segment, 0x100 = last, 2 = AES-IV descriptor, 0x30003 first AES data seg / 0x10003 continuation; 32 descriptors per chain | `cdma.c:14-20,107-114,219-298` |
| Peripheral map | 0x80000020 (id 0), 0x800000A0/A4 (id 1; SDIO), 0x81000020 (id 0; CE-ATA), 0x81200014/18 (ids 1,2; H2FMI0 data/meta), 0x81300014/18 (ids 3,4; H2FMI1) | `cdma.c:51-60` |
| Channel assignment | 1 = AES in, 2 = AES out; 5/6 = H2FMI0 data/meta; 7/8 = H2FMI1; AES filter contexts 2..8 | `cdma.c:554-557,645`, `h2fmi.c:326-337`, `cdma.c:327` |
| IRQs | 0x30 + channel; gate 0x14 | `dma.h:13`, `cdma.c:70` |
| AES ctx n regs (0x87800000 + n<<12) | +0x00 setup, +0x10..0x1C IV (only ctx1 IV written: 0x87801010..1C), +0x20..0x3C key words 0..7 | `dma.h:21-29`, `cdma.c:578-586,611-620` |
| Setup word | bits8-15 = DMA channel; bit16 = encrypt (0 = decrypt), bit17 = enable; bits18-19 keylen 0/1/2 = 128/192/256; bit20 = custom key in regs; bit21 = GID; bit22 = UID (in `dma_set_aes`); `aes_hw_crypto_cmd` instead sets GID as bit21\|bit19 and UID as *no key bits* ("still broken" comment); +0x00 of ctx0 bit0/bit1 = UID/GID disabled | `cdma.c:343-393,564-575,627-643` |
| openiBoot key-type API | low 12 bits: 0 = custom, 512 = GID, 513 = UID; bits28-31 = keylen | `aes.c:107-131` |
| Derived keys | Key835/89B/836/838 = UID-AES of constants (`Gen835`=0x01×16 …) | `aes.c:10-36` |
| iEmu | ctx setup decoded as bit20→custom, bit21→GID, else UID; AES on channel-2 start; segments read via `segmentBuffer{address,flags,buffer,size,iv[4]}`; IRQ ack bit19 | `IEMU/s5l8930.c:601-1049`, `s5l8930.h:228-254` |

#### 1.11 AES standalone / SHA1
- No standalone AES block on A4 in openiBoot; all AES is DMA-AES above. (S5L8720 has a standalone AES at 0x38C00000 with CONTROL/GO/KEYLEN/INADDR/OUTADDR/KEY 0x4C/TYPE 0x6C/IV 0x74 — `plat-s5l8720/includes/hardware/aes.h:5-23` — **different**.)
- SHA1: openiBoot A4 uses **software** SHA1 (`h2fmi.c:15,2086-2090`). iEmu maps a SHA1 engine at 0x80100000, size 0x100: input words at +0x40..0x7C, digest at +0x20..0x30, reset on reading +0x30 (`IEMU/s5l8930.h:127`, `s5l8930.c:1115-1197`). Register semantics unverified against any driver.

#### 1.12 USB OTG (Synopsys DWC) + PHY
| Item | Value | Source |
|---|---|---|
| OTG base | 0x86100000, IRQ 0xD, gates OTG 0x18 / PHY 0x1D | `usb.h:18,29-31`; `IEMU/s5l8930.h:50-51` |
| FIFO | RX 0x11B, TX 0x100 @0x11B, periodic 0x100 @0x21B, turnaround 5 | `usb.h:47-52,34` |
| Standard DWC regs | GOTGCTL 0, GOTGINT 4, GAHBCFG 8, GUSBCFG 0xC, GRSTCTL 0x10, GINTSTS 0x14, GINTMSK 0x18, GRXFSIZ 0x24, GNPTXFSIZ 0x28 … | `usb.h:62-70` |
| Init | gates on → PCGCCTL on → DCTL soft-disconnect → `usb_phy_init()` → read GHWCFG1-4 → core reset | `OIB/usb-synopsys/usb.c:243-272` |
| PHY base | 0x86000000: OPHYPWR 0, OPHYCLK 4 (bits0-1: 0=12,1=24,2=48 MHz,3=other), ORSTCON 8 (bit0 PHY sw reset), +0x1C (=6), +0x44 (=0x733), +0x48 charger-detect bits1-2, +0x60 (=0x200) | `usbphy.h:4-41`, `usbphy.c:7-43`, `pmu.c:319-326` |
| iEmu PHY | 0x86000000 size 0x40, stores 0/4/8/0x20 | `IEMU/s5l8930.c:1243-1319` |

S5L8720 PHY at 0x3C400000 has the same OPHYPWR/OPHYCLK/ORSTCON/0x1C/0x44 layout (`plat-s5l8720/includes/hardware/usbphy.h:4-12`, value 0xE3F vs 0x733) and 8720 OTG at 0x38400000 is the same DWC core → **reusable** with new bases/gates.

#### 1.13 Chip ID / fuses
CHIPID base 0xBF500000 (`chipid.h:5`, `IEMU/s5l8930.h:58`). +0: bits4-5 = GPIO epoch, bits9-15 = power epoch (`chipid.h:12-13`); +4: bits24-27 = SPI clock type, bit31 set by `timer_setup` (`chipid.h:8,11`, `timer.c:46`). NAND epoch = power epoch or 1 if 0 (`chipid.c:17-24`). iEmu returns +0 = 0x31800587 (power epoch 2, GPIO epoch 0), +8 = 0xF6A15B30, +0xC = 0x47002735 (`IEMU/s5l8930.c:1056-1063`); POWER_ID 0x2020001 → epoch 2 (`s5l8930.c:132-133`). `miu_setup` fails if `POWER_ID>>24 != chipid power epoch` (`miu.c:8-13`). **NOT FOUND**: security epoch/board-id bit positions for K48, ECID/UID fuse registers, a documented "chip id = 0x8930" register.

#### 1.14 Watchdog, RTC, PMU, MISC
- WDT: `WDT_CTRL 0x3E300000`, `WDT_CNT 0x3E300004`, IRQ 0x33 (`a4.h:39-42`) — S5L8920 values, plat-a4 has no `wdt.c` and builds `MALLOC_NO_WDT`; unverified for 8930.
- RTC: none; time comes from the 24 MHz tick counter (`timer.c:216-238`).
- PMU (I2C0 0xE8/E9): regs 0x2 ADC, 0x7 power supply, 0x12 OOCSHDWN, 0x30/0x31 muxsel/adcval, 0x50-0x59 GPIO, 0x1D+ LDO V, 0x5F LDO gates, 0x8D/0x8E battery V, GP memory via `reg^0x80` (0..0xF), backlight 0x66/0x67/0x68 (iPhone4) (`pmu.h:7-35`, `pmu.c:27-47,259,282`). iPad backlight instead: PWM block 0xBF600000 (+0 period, +0x14 =3 then poll bit0, +0x18 duty) gate 0x3E + GPIO 0x1403 (`clcd.c:779-790`).
- iEmu "MISCSYS" 0xBF800000: +0x144/+0x184 return 3 (`IEMU/s5l8930.h:55`, `s5l8930.c:249-257`) — purpose unknown.

#### 1.15 IOP
openiBoot A4 does **not** use the IOP; only comments "Somewhere in IOP" in the H2FMI struct (`OIB/plat-a4/includes/h2fmi.h:88-89,116-117`). iEmu models it as a second Cortex-A8 with its own 4 PL192s at 0xBF300000 + n·0x10000, control block at 0x86300000 (+0x100: bit0 start, bit4 stop; +0x110 entry address; +0x18/0x1C/0x24 ignored), IOP→main IRQ 3, H2FMI0/1 and CDMA ch5-8 IRQs rerouted to the IOP VIC, IOP timer on IRQ 5; it NOPs 8 halfwords at 0x40762790 to skip `pmgr_enable_gates` in iBoot (`IEMU/s5l8930_iop.c:18-21,50-69,80-104,227-269`).

#### 1.16 Display pipe / CLCD / MIPI-DSIM
| Block | Base | Source |
|---|---|---|
| DISPLAY_PIPE0 / PIPE1 | 0x89000000 / 0x89100000 (iPad uses PIPE0; iEmu maps "CLCD" at 0x89100000 and just assumes FB=0x4F700000) | `clcd.h:4-5,10`, `IEMU/ipad1g.h:13-14`, `ipad1g.c:251-256` |
| CLCD (timing gen) | 0x89200000; RGBOUT 0x89600000 (ATV) | `clcd.h:6-7` |
| MIPI DSIM | 0x89500000, gate 0x11; regs STATUS 0 … PHYACCHR1 0x58 (Samsung DSIM layout, identical to 8720's at 0x3D800000) | `mipi_dsim.h:5-32`; 8720 `plat-s5l8720/includes/hardware/mipi_dsim.h:5-36` |
| Pipe regs used | +0x104C (\|=0x10, bits8-10=1, bits16-26=0x40), +0x1030 = W<<16\|H, +0x205C = (0x180<<16)\|0x1F0 for PIPE0, +0x2060=0x90, +0x105C=0x13880801, +0x2064 underrun color, +0x1038 \|=0x100, +0x2040=0xFFFF0202, UI0 window +0x4040 fmt\|1, +0x4044 fb addr, +0x4048 stride&~0x3F\|2, +0x4050=0, +0x4060 W<<16\|H, +0x404C=1, +0x4074=0x200060, +0x4078=32 | `clcd.c:260-274,723-733` |
| CLCD regs | +0 (0x100 reset, poll, then 4), +4=3, +0x14=0x80000001 (\|0x1110000 for ≤18bpp), +0x18=0x20408, +0x50 bit0 enable / bit2 stopped, +0x54 VIDCON1, +0x58 VIDTCON0(v), +0x5C VIDTCON1(h), +0x60 (W-1)<<16\|(H-1), +0x34/+0x38 gamma window write | `clcd.c:276-289,169-177,607-617` |
| iPad "k48" panel | 1024×768, 18 bpp, dot clock 0x413B380 = 68.4 MHz, HBP/HFP/HSW 0x85/0x85/0x87, VBP/VFP/VSW 0xA/0xA/0xC, unkn18 = 0x644 (4 lanes, PLL bypass path); panel-ID read via DCS 0xB1; reset GPIO 0x1404 | `clcd.c:72,394-426`, `mipi_dsim.c:93-154` |
| LCD clock | divisors on clock indices 14 (PREDIV4) and 36 (VID0) from EPLL (`CalculatedFrequencyTable[3]`) | `clcd.c:204-236` |

#### 1.17 SDIO, CE-ATA, H2FMI NAND
- SDIO 0x80000000, SDHC-standard register block (SDMAADD 0, BLK 4, ARGU 8, CMD 0xE, RESP 0x10-0x1C, DBUF 0x20, STATE 0x24, HOSTCTL 0x28, CLKCON 0x2C, SWRESET 0x2F, IRQ 0x30/0x34/0x38, CAP 0x40/0x44, ADMA 0x54, INFO 0xFC, ctrl2/3/4 at 0x80/0x84/0x8C, version 0xFE); IRQ 0x26, gate 0x24, base clk 51.3 MHz (`sdio.h`, `sdio.c:13-54,587-613`).
- CE-ATA: 0x81000000 (iEmu unmapped stub, `IEMU/s5l8930.c:1493`; DMA FIFO 0x81000020 `cdma.c:55`).
- H2FMI0/1: 0x81200000 / 0x81300000, IRQ 0x22/0x23, gates 0x27/0x29 (+1 each also switched) (`h2fmi.h:4-10`). Register windows (`OIB/plat-a4/includes/hardware/h2fmi.h:15-41`, iEmu names in `IEMU/s5l8930_h2fmi.c:238-270`): controller +0x0 ECC/page fmt, +0x4 CCMD (6=reset, 3/5 start), +0x8 ccmd status, +0xC CSTS (0xF ack), +0x10 CREQ, +0x14 DATA0, +0x18 DATA1, +0x1C DMA status (bit 0x18 = ready), +0x34 PAGEFMT; NAND +0x40000 reset(1), +0x40008 timing, +0x4000C chip-enable mask, +0x40010 NREQ, +0x40014 NCMD (0xFF reset, 0x90 READID, 0x3000 read, 0x70 status…), +0x40018/+0x4001C address, +0x40020 addr-mode, +0x40024, +0x40040, +0x40044 NSTS (write-1-clear, poll), +0x40048 status byte, +0x4004C; ECC +0x80008 cfg (`(bits<<8)|0x20000`), +0x8000C, +0x80010 (0x1A8), +0x80014. Probe: `h2fmi_init` → init both buses (gates, timing 0xFFFF, gate-reset, +4=6, +0x40000=1) → reset+READID on all 16 CEs → match against iPad chip table (`h2fmi.c:120-130`, IDs 0x7294D7EC, 0x72D5DEEC, 0xB614D5AD, 0x2594D7AD, 0x3294E798, 0x3294D798, 0x3295DE98, 0x3295EE98, 0x4604682C) and board table (`h2fmi.c:183-202`) → compute ECC/timing → set +0x40008 → VFL/FTL detect (`h2fmi.c:3018-3243`). iPad reads EMF/Dkey from the NOR "PLog" at 0xFA000 instead of NAND (`h2fmi.c:2896-2949`). Data whitening PRNG seed 0x50F4546A (`h2fmi.c:3220-3232`).

### 2. openiBoot A4 boot-time init order (what an emulator must satisfy)
`OpenIBootStart` → `platform_init()` → `init_setup()` → boot modules → console (`OIB/openiboot.c:45-56`). `platform_init` (`OIB/plat-a4/a4.c:23-64`):
1. `arm_setup()` (A8 AuxControl bit3 speculative AXI, `arch-arm/arm.c:49-50`), `mmu_setup()` (identity map, RAM cacheable, 0xC0000000→RAM alias, `mmu.c:43-57`), `tasks_setup()`.
2. `miu_setup()`: read 0xBF106000 and 0xBF500000, compare epochs (`miu.c:8-16`).
3. `power_setup()`: no-op on A4 (`power.c:5-20`).
4. `clock_setup()`: write 48 clock-config regs at 0xBF100040.. (index 48 first then 47→1, then index 0), read PLL CON0s to compute frequencies, read ARMCLK multiplier (`clock.c:393-424`). Emulator must return sane PLL CON0 values with ENABLE bit set (else freq=0 → UART baud div-by-zero) and echo written clock regs.
5. `interrupt_setup()` on 4 VICs (`interrupt.c:7-40`).
6. `gpio_setup()`: strap read + POWER_ID write, install IRQ 0x74 (`gpio.c:80-122`).
7. `timer_setup()` (CHIPID+4 bit31, 0xBF1010xx/0xBF1012xx writes), `event_setup()` (0xBF102010=2, IRQ 6) (`timer.c:41-60`, `event.c:18-26`).
8. `uart_setup()`: gates 0x30-0x36, program 7 UARTs, IRQs 0x16-0x1C (`uart.c:124-193`).
9. `i2c_setup()`: gates 0x39-0x3B, bit-bang SCL 19× via GPIO, +8=0x30, +0xC=0x37, IRQs 0x13-0x15 (`i2c.c:22-52`).
10. `dma_setup()`: gate 0x14 (`cdma.c:69-73`).
11. `spi_setup()`: SPI0-2 gates + control=0 + IRQs (`spi.c:359-383`).
12. `LeaveCriticalSection()`, then `aes_setup()`: 4 UID-key AES ops through CDMA channels 1/2 + AES ctx 1 — **requires functional CDMA+DMA-AES and UID key** (`aes.c:22-36`, `cdma.c:550-672`).
13. `displaypipe_init()` (gates 0xB,0x12,0xF; pipe/CLCD regs; `pinot_init` → MIPI DSIM PLL/lane bring-up with busy-waits on STATUS bits 31,20,8-9,10; DCS panel-ID read) (`clcd.c:238-381,387-468`, `mipi_dsim.c:84-208`).
14. `pmu_setup_gpio(0,1,1)`, `pmu_setup_ldo(10,1800,0,1)` over I2C0 (`a4.c:62-63`).
Then MODULE_INIT: `h2fmi_init` (NAND probe, `h2fmi.c:3244`), `nor_init` (SPI0 JEDEC id, `nor-spi/nor.c:540`), `accel_init` (I2C2 WHOAMI, `accel.c:50`), `pmu_init` (I2C0 GP-mem write), USB `usb_setup` (`usb-synopsys/usb.c:243-272`).

### 3. Overlap with S5L8720 (qemu-ios iPod touch 2G models)
| Block | S5L8720 (openiBoot `plat-s5l8720/includes/hardware/*`, qemu-ios `include/hw/arm/ipod_touch_2g.h`) | S5L8930 | Register layout same? / reuse |
|---|---|---|---|
| VIC | 2× PL192 @0x38E00000/0x38E01000 (`interrupt.h:13-14`) + EDGEIC | 4× PL192 @0xBF200000+n·0x10000, no EDGEIC | **Same IP**; reuse `hw/intc/pl192` with 4 instances chained |
| Timer | 0x3C700000, 8 timers, ticks at +0x80/0x84, IRQLATCH 0x118, IRQ 7, gate 0x13 (`timer.h:8-39`) | 0xBF100000, 7 timers, ticks at +0x2000/+0x2004, event count/state at +0x2008/+0x2010 (IRQ 6) and +0x200C/+0x2014 (IRQ 5), IRQLATCH 0xF8 | Per-timer block (CONFIG/STATE/COUNT/COUNT2/PRESCALER) identical; tick/event registers relocated → adapt `ipod_touch_timer.c` with new offsets |
| Clock/PMGR | 0x3C500000 CONFIG0/1/2, PLL0-2 CON @0x20-0x28, gates bitmask regs 0x48/0x4C/0x58/0x68/0x6C (`clock.h:14-32`) | PMGR 0xBF100000, 4 PLLs, 48 config regs, per-gate regs at +0x1010 | **Different**; new model needed (iEmu `s5l8930_pmgr_*` is a starting point) |
| GPIO | GPIO 0x3CF00000 (per-port DAT/PUD, FSEL @0x1E0) + GPIOIC in SYSIC 0x39700000, group IRQs 0x21/0x20/0x1F/3/2 (`gpio.h`, `plat-s5l8720/gpio.c:34-59,166`) | 0xBFA00000 per-pin 32-bit regs, GPIOIC at +0x800/0x840/0x880/0xC00, single IRQ 0x74 | **Different**; new model |
| UART | 0x3CC00000, 5 ports stride 0x4000, gate 0x29 (`uart.h:5-29`) | 0x82500000, 7 ports stride 0x100000 | **Same registers/bits** → reuse qemu-ios UART with new bases/stride/IRQs |
| SPI | 0x3C300000/0x3CE00000/0x3D200000/0x3DA00000/0x3E100000, regs 0x0-0x4C (`spi.h:5-26`) | 0x82000000+n·0x100000, same regs | **Same** (tx fifo 0x1F vs 16) → reuse `ipod_touch_spi.c` + `ipod_touch_nor_spi.c` (NOR on SPI0/CS 0x505 vs 8720 SPI1/CS 0x406) |
| I2C | Samsung IICCON/IICSTAT/IICADD/IICDS (`i2c.h:9-17`) | new FIFO-style controller (addr/status/reg/len/data/cmd at 0/0xC/0x10/0x18/0x20/0x24) | **Different**; new model (iEmu `s5l8930_i2c.c` sketch) |
| USB OTG | DWC @0x38400000, IRQ 0x13, FIFO 0x11B/0x100 (`usb.h:20,36-38,47-52`) | DWC @0x86100000, IRQ 0xD, identical FIFO config | **Same** → reuse `ipod_touch_usb_otg.c`; PHY (`ipod_touch_usb_phys.c`) same layout at 0x86000000 |
| AES | standalone 0x38C00000 (`aes.h`) | DMA-AES inside CDMA at 0x87800000 | **Different**; new CDMA+AES model required (also replaces the 8720's PL080 DMACs) |
| SHA1 | 0x38000000 (qemu-ios `ipod_touch_sha1.c`, IRQ 0x28) | 0x80100000 per iEmu (unverified) | Possibly similar (input regs, digest readout) — unverified; treat as new |
| MIPI DSIM | 0x3D800000, 2 lanes (`mipi_dsim.h:6,47`) | 0x89500000, 4 lanes, same register file | **Same** → reuse `ipod_touch_mipi_dsi.c` |
| LCD | S5L8720 CLCD 0x38900000 (`clcd.h:4`) | display pipe 0x89000000 + CLCD 0x89200000 (new register set) | **Different**; new model |
| ChipID | 0x3D100000 (`chipid.h:5-14`) | 0xBF500000, different bit fields | trivial new model |
| WDT | 0x3C800000 (`s5l8720.h:32`) | unknown (a4.h copies 0x3E300000 from 8920) | NOT FOUND |
| SDIO | 0x38D00000 SDHC (`ipod_touch_sdio.c`) | 0x80000000 SDHC | Likely same SDHC core → reuse with new base |
| NAND | FMSS 0x38A00000 | H2FMI 0x81200000/0x81300000 | **Different**; new model (iEmu `s5l8930_h2fmi.c` is a partial one) |

### Explicitly NOT found
SRAM base/size; K48 board-id / security-epoch fuse bit definitions; ECID/fuse registers; SHA1 register semantics from a driver; a WDT address for S5L8930; multitouch SPI port/GPIOs for iPad; IOP register semantics beyond iEmu's 0x86300000 start/stop stub; display-pipe/CLCD/MIPI IRQ numbers; AMC (DRAM controller) register map (openiBoot only maps 0x84000000–0x84400000 cacheable, never programs it); the meaning of the `timer_setup` writes to 0xBF1010xx/0xBF1012xx and of MISCSYS 0xBF800000.