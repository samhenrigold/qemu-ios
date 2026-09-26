# iPad 1G (K48AP / S5L8930) board-level device inventory — as seen by openiBoot, the decrypted 7B367 DeviceTree, iBoot-817.28 strings, and iEmu

Scratch artifacts produced (read-only elsewhere): `/tmp/claude-0/-home-user/2fb4038f-ca86-59b7-95f7-7e88ca8c44ec/scratchpad/dec/` — `DeviceTree.k48ap.bin` (decrypted), `dt.txt` (parsed dump, 1085 lines), `iBoot.k48ap.bin` + `iboot.str`, `LLB.k48ap.bin`, `dtdump.py`, `img3data.py`. Keys from https://api.ipsw.me/v4/keys/ipsw/iPad1,1/7B367 (DeviceTree iv `0e3fdb2c…`, key `2b5a6118…`; iBoot iv `36e1bcd0…`, key `1e3a1ca2…`). Note: DT "reg" addresses are relative to `arm-io` `ranges = 0x0→0x80000000 (0x40000000), 0x40000000→0x40000000` (dt.txt:102), so DT `0x09000000` = phys `0x89000000`, `0x3f200000` = `0xBF200000`.

## 1. openiBoot build target for iPad1G

- Target: `plat-a4/iPad1G.SConscript:1-21` — sources = `plat_a4_src` + `#audiohw-null.c` + `accel.c`; `CPPDEFINES OPENIBOOT_VERSION_CONFIG=" for iPad 1G", MACH_ID=3593`; module `nor-spi`; `env.OpenIBootTarget('iPad1G','ipad_1g_openiboot','CONFIG_IPAD_1G',…)`.
- Platform: `plat-a4/SConscript:5-6` defines `ARM_A8, CONFIG_A4, MALLOC_NO_WDT, BIG_FONT`; modules `acm, usb-synopsys, vfl-vfl, vfl-vsvfl, ftl-yaftl` (:8-14); sources `a4.c aes.c buttons.c chipid.c clock.c event.c gpio.c i2c.c interrupt.c mipi_dsim.c clcd.c miu.c mmu.c pmu.c power.c spi.c timer.c uart.c cdma.c usbphy.c h2fmi.c sdio.c` + hfs (:16-40).
- Init order: `openiboot.c:33 init_modules(); :47 platform_init(); :51 init_boot_modules()`; `plat-a4/a4.c:23-64`: arm/mmu/tasks → miu, power, clock → interrupt → gpio → timer, event → uart, i2c → dma → spi → aes → `displaypipe_init()` + `lcd_set_backlight_level(1500)` → `pmu_setup_gpio(0,1,1); pmu_setup_ldo(10,1800,0,1)`.
- **Not compiled for A4 at all**: multitouch (`plat-s5l8900/multitouch-z1.c`, `multitouch-z2.c` only), WLAN firmware upload (`plat-s5l8900/wlan.c` only), audio codec (`audiohw-null.c`), ALS (`plat-s5l8900/als-*.c`), camera (`plat-s5l8900/camera.c`). `grep multitouch|wlan|audiocodec plat-a4/` → only `#include "audiocodec.h"` in `a4.c:21`.
- CONFIG_IPAD_1G-specific code sites: `gpio.c:27,52-76` (own 176-entry GPIO reset table = 22 ports × 8), `mipi_dsim.c:156-161`, `clcd.c:394-405,461-466,478-482,756-790`, `h2fmi.c:120-130,183-200,257-275,878-880,2896-2946`, `clcd.h:38-39`, `mipi_dsim.h:166-167`, `vsvfl.c:1127-1131`.

## 2. SoC memory map (openiBoot headers vs DT)

| Block | Address | Source |
|---|---|---|
| RAM | 0x40000000–0x50000000 (256 MiB, iPad; iPhone4 0x60000000) | `hardware/a4.h:10-15`; DT `memory reg` is 0 (filled at boot) |
| LargeMemoryStart / heap | 0x46000000 / +0x02000000 | `a4.h:16,25` |
| PageTable | RAMEnd-0x8000 | `a4.h:26` |
| VIC0-3 (PL192) | 0xBF200000 stride 0x10000, 4×32 irqs | `interrupt.h:13-16`; dt.txt:119,126 |
| PMGR/clock/timers | 0xBF100000 (PLL con 0x00-0x24, CON0 0x30, gates base 0x1010, timers 0xBF102000, TIMER_REGISTER 0xBF102008/TICK 0xBF102010, IRQ 6) | `clock.h:9-61`, `timer.h:8-40`; dt.txt:150 |
| GPIO | 0xBFA00000, pin reg = (8*port+pin)*4, INT regs 0x800/0x840/0x880/0xC00, IRQ 0x74, 22 ports, 7 int groups | `gpio.h:5-24`, `gpio.c:238,253`; dt.txt:137-139 |
| CHIPID | 0xBF500000 | `chipid.h:5`; dt.txt (arm-io reg 0xbfc00000) |
| WDT | 0x3E300000 | `a4.h:39-40` |
| SWI (backlight serial-wire) | 0xBF600000 | `clcd.c:782-784`; dt.txt:962 |
| CDMA + AES | 0x87000000 (0x26000) / 0x87800000 (0x9000), 37 channels, IRQs 0x31… | `dma.h:7,21`; dt.txt:166-170 |
| SHA1 | 0x80100000 | dt.txt:254 |
| SDIO | 0x80000000 (SDHCI regs `sdio.h:10-40`), IRQ 0x26, clockgate 0x24 | `sdio.h:5-6`; dt.txt:241,247 |
| CE-ATA | 0x81000000 | dt.txt:270 |
| H2FMI0/1 | 0x81200000 / 0x81300000 (+0x40000 NAND regs, +0x80000 ECC regs), IRQ 0x22/0x23 | `hardware/h2fmi.h:4-10,15-42`; dt.txt:274,280 |
| SPI0-4 | 0x82000000 + n*0x100000, IRQ 0x1D-0x21, gates 0x2B-0x2F | `spi.h:5-21`, `spi.c:50-56` |
| UART0-6 | 0x82500000 + n*0x100000, IRQ 0x16 (uart0), 0x17… | `uart.h:7-34`; dt.txt:409-512 |
| PKE | 0x83100000 | dt.txt:531 |
| I2C0/1/2 | 0x83200000/0x83300000/0x83400000, IRQ 0x13/0x14/0x15 | `i2c.h:5-7`, `i2c.c:12-14`; dt.txt:537,544,641,648 |
| PWM | 0x83500000 (codec MCLK) | dt.txt:707-712 |
| AMC (DRAM ctrl) | 0x84000000/0x84100000/0x84300000 | `a4.h:34`; dt.txt:1006 |
| I2S0/1/2 | 0x84500400/0x84501400/0x84502400 | dt.txt:717,736,757 |
| DisplayPort | 0x84900000 | dt.txt:1014 |
| VXD / SGX535 | 0x85000000 / 0x85100000 | dt.txt:858,869 |
| USB PHY / OTG(dev) / EHCI / OHCI0 / OHCI1 | 0x86000000 / 0x86100000 (IRQ 0xd) / 0x86400000 (0xe) / 0x86500000 (0xf) / 0x86600000 (0x10) | `usbphy.h:4`; `usb-synopsys/includes/hardware/usb.h:18`; dt.txt:778-837 |
| IOP (ARM7 coproc for NAND) | 0x86300000 + VIC 0xBF300000, IRQ 3 | dt.txt:848; iEmu `s5l8930_iop.c:20,255` |
| VENC / JPEG / ISP / DART1 | 0x88000000 / 0x88200000 / 0x88300000 / 0x88D00000 | dt.txt:880,891,897,179 |
| DISPLAY_PIPE0 / DISPLAY_PIPE1 / CLCD / TV-OUT / MIPI-DSIM / RGBOUT / DART2 / SCALER | 0x89000000 / 0x89100000 / 0x89200000 / 0x89400000 / 0x89500000 / 0x89600000 / 0x89D00000 / 0x89300000 | `clcd.h:4-7`, `mipi_dsim.h:5`; dt.txt:924,939,974,986,211,917 |

**Important RAM observation**: decrypted iBoot-817.28 k48ap is linked at **0x5FF00000** (verified: LCD table entry at payload offset 0x29050 has name pointer 0x5FF22360 → "k48" string at offset 0x22360). With 256 MiB physical RAM this implies DRAM aliases across 0x40000000–0x60000000; iEmu therefore maps RAM_SIZE = 0x20000000 at 0x40000000 and again at 0xC0000000 (`teknogeek/iemu/hw/ipad1g.h:4-11`, `ipad1g.c:422-427`). openiBoot's iPad framebuffer constant is 0x4F700000 vs iPhone4 0x5F700000 (`clcd.h:14-18`).

## 3. PMU / charger / fuel gauge

- **PMU = Dialog "D1815"** (Apple 338S0805): DT `/arm-io/i2c0/pmu compatible='pmu,d1815'`, `reg = 0x74` (7-bit) on **I2C0**, IRQ = GPIO 0x0D (dt.txt:546-574). Properties: `no-backlight` (dt.txt:556 — backlight is NOT PMU-driven on iPad), `lcm-boost-direct-control`, `swi-vcores`, `gpio-pin-config` (10 PMU GPIOs), event names gpio3=ioxpander, gpio6=battery, gpio7=land_charger, gpio9=port_charger, gpio10=mikey (dt.txt:552-569), `function-keepact = GPIO 0x203`, `function-panic_reset = pmgr WDT`.
- openiBoot addressing: `PMU_I2C_BUS 0`, 8-bit `PMU_SETADDR 0xE9`/`PMU_GETADDR 0xE8` (`hardware/pmu.h:5-8`; `i2c.c:105` writes `addr>>1`).
- Register map used (`hardware/pmu.h:11-35`, `pmu.c`): ADC status `0x02` (bit 0x20 = done), power-supply `0x07` (0x08 USB, 0x10 FireWire), OOCSHDWN `0x12` (shutdown/reboot, `pmu.c:201-230`), MUXSEL `0x30` (write `mux|0x10`, mux 3 needs `|0x20`), ADCVAL `0x31-0x32` (result `buf[1]<<4|buf[0]&0xF`, `pmu.c:288-317`), battery V `0x8E/0x8D` (`pmu.c:371-373`), LDO enable regs `0x14/0x15/0x16/0x18`, LDO voltage `0x1D+idx`, LDO gates `0x5F`, 19 LDOs (`pmu.c:27-47,88-151`), PMU GPIO regs `0x50-0x59` (`pmu.c:61-86`: 0x40=input, bit1=level), GP-mem/"NVRAM" regs accessed as `reg^0x80` (`pmu.c:259,282`) with `PMU_IBOOTSTATE 0xF, IBOOTDEBUG 0, IBOOTSTAGE 1, IBOOTERRORCOUNT 2, IBOOTERRORSTAGE 3` (`includes/pmu.h:44-48`). USB-charger ID via USB PHY `OPHYUNK3` (0x86000048) bits 1-2 + ADC mux 6 (`pmu.c:319-358`). iBoot strings confirm Dialog driver: `dialog_read_adc timeout, MUX_SEL=%x`, `kDIALOG_SYS_CONTROL` (iboot.str:656,659). RTC: openiBoot A4 has no RTC read (`pmu_get_epoch` declared `includes/pmu.h:63` but not implemented in plat-a4).
- **Charger = LTC4099** on I2C0 addr 0x09 (dt.txt:605-618: brick_id_n/p GPIO 0xC01/0xC02, ddis_chg 0x1303, hi_i 0x1207) and a second at I2C2 0x09 (dt.txt:672-680); `/charger compatible 'charger,k48'`, interrupt via TCA6408 (dt.txt:1048-1067).
- **Fuel gauge = bq27545 over HDQ on UART5** (0x82A00000): `compatible` bytes decode "gas-gauge,bq27545\0gas-gauge,hdq", `function-battery_swi = pmu GPIO 5` (dt.txt:482-502); iBoot strings `hdqgauge voltage bogus`, `gas gauge SWI line low` (iboot.str:736-740).
- **GPIO expander = TCA6408** on I2C0 addr 0x20, IRQ GPIO 0x11 (dt.txt:575-588); its pins: 0=bt_reset, 1=**wlan/sdio device_reset**, 2=bluetooth, 3=wlan, 4=baseband, 5=firewire (dt.txt:240,473,577-584).

## 4. Display (1024×768, MIPI-DSI "pinot" panel, CLCD + DISPLAY_PIPE0)

- DT: `clcd reg 0x89000000/0x7000 + 0x89200000/0x2000`, IRQs 0x2A,0x29, `dma-channels ch 0x25 @0x8910103C`, `iommu-parent = dart2/mapper-clcd` (dt.txt:919-934); `mipi-dsim 0x89500000`, `#lanes 4`, IRQ 0x28 (dt.txt:935-947); `mipi-dsim/lcd compatible 'lcd,pinot'`, `function-reset = GPIO 0x1404`, `function-lcd_ldo = GPIO 0x1301` (dt.txt:948-955). **iOS drives CLCD through a DART IOMMU** (dt.txt:203-220) — the kernel may program IOVAs into the pipe; qemu-ios needs either DART emulation or identity mapping.
- **Panel timing table** (`clcd.c:72`, `DISPLAYID 1` from `clcd.h:38-39`; LCDInfo layout `includes/mipi_dsim.h:7-27`): `{"k48", 0xA, 68400000 Hz, DotPitch 0x84, w 1024, HBP 0x85, HFP 0x85, HSPW 0x87, h 768, VBP 0xA, VFP 0xA, VSPW 0xC, IVClk 0, IHSync 0, IVSync 0, IVDen 0, bpp 18, type 3(pinot), 0x644}`. **Byte-identical table found in decrypted iBoot** at payload offset 0x29050 (`0000000a 0413b380 00000084 00000400 00000085 00000085 00000087 00000300 0000000a 0000000a 0000000c 0 0 0 0 00000012 00000003 00000644`). 0x644 → 4 data lanes, ESC prescaler 0x64, PLL p/m/s = 0 → PLL-bypass path (`mipi_dsim.c:93-100,150-154`). Refresh ≈ 68.4e6/((1024+0x85+0x85+0x87)*(768+10+10+12)) ≈ 59.6 Hz.
- **displaypipe_init sequence** (`clcd.c:238-381`): gate 0xB (HPERF2), 0x12 (DISPLAY_PIPE), 0xF on (:243-250). DISPLAY_PIPE writes: `+0x104C |= 0x10; (&0xFFFFF8FF)|0x100; (&0xF800FFFF)|0x4000000` (:260-262); `+0x1030 = w<<16|h` (:263); `+0x205C = (0x180<<16)|0x1F0` for pipe0 (:264-271); `+0x2060 = 0x90`; `+0x105C = 0x13880801`; `+0x2064 = 0x80000000|b(0x3FF)` underrun colour (:272-274). CLCD writes: `+0x00 = 0x100` then poll until bit 0x100 clears (soft reset), `+0x00 = 4`, `+0x04 = 3`, `+0x14 = 0x80000001` (`|= 0x1110000` if bpp<=18 → set for iPad), `+0x18 = 0x20408`, `+0x50 = 0`, `+0x54 = VIDCON1 (IVClk<<3|IHSync<<2|IVSync<<1|IVDen)`, `+0x58 = (VBP-1)<<16|(VFP-1)<<8|(VSPW-1)`, `+0x5C = same for H`, `+0x60 = (w-1)<<16|(h-1)` (:276-289). Clock: `clock_set_divisor(14, div1); clock_set_divisor(36, div2)` from PLL2 (:204-236). Then `createWindow` (:331), `configureLCDClock`, `pinot_init` (:338-344), `lcd_fill_switch(ON)` sets `CLCD+0x50 |= 1` (:169) — OFF clears bit0 and polls `CLCD+0x50 & 2` (:176-177); gamma LUTs written via `CLCD+0x34 = (win<<12)|0x10000`, 256× `CLCD+0x38 = val|1<<31`, `CLCD+0x34 = 0` for windows 4,5,6 (:597-618) with iPad panel gamma descriptors `{0xB30689, mask 0xFFFFBF}` and a second iPad entry (`clcd.c:28-43`).
- **Framebuffer / window registers** (`createWindow`, `clcd.c:698-737`): buffer = `memalign(16, w*bpp/8*h)` (heap; iBoot instead uses top-of-RAM 0x4F700000 → `clcd.h:17`), `DISPLAY_PIPE+0x4040 = (fmt<<8)|1` (fmt 0 = RGB888/32bpp, 4 = RGB565), **`+0x4044 = framebuffer physical address`**, `+0x4048 = (lineBytes & ~0x3F) | 2` (stride), `+0x4050 = 0`, `+0x4060 = w<<16|h`, `+0x2040 = 0xFFFF0202` (blend), `+0x404C = 1`, `+0x4074 = 0x200060`, `+0x4078 = 32`, `+0x1038 |= 0x100` (enable UI0 layer). Register block naming from `cmd_clcd_dump` (`clcd.c:487-514`): +0x1000 control (24 regs), +0x2000 blend (26), +0x3000 video, +0x4000 UI0 (31), +0x5000 UI1 (31). Default colourspace RGB888 (`clcd.c:329`; iBoot reads nvram `display-color-space`, iboot.str:592) and iBoot exports the address via nvram var `framebuffer` (`clcd.c:335-336`, iboot.str:595) and DT `vram` node (dt.txt:95-98).
- **pinot_init** (`clcd.c:387-468`): GPIO 0x1404=0 (reset), `mipi_dsim_init`, GPIO 0x1404=1, sleep 6 ms, DCS short write `mipi_dsim_write_data(5,0,0)`, HS clock on (`CLKCTRL|=1<<31`, wait `STATUS&HS_READY(1<<10)`, `mipi_dsim.c:21-29`), 25 ms, **panel-ID read**: packet `data_id 0x14, data0 0xB1` expecting ≥3 bytes; panel_id = `b0<<24|b1<<16|(b3&0xF0)<<8|(b2&0xF8)<<4|(b3&0xF)<<3|(b2&7)`, default colour from `b3&8`, backlight cal = `b5`, `b4` saved (:412-425). If `(id>>8&0xFF)` ∈ {3,4,7,8} → `MDRESOL |= 1<<31` early (:432-435). Then DCS 0x11 (sleep out), wait 7 frames, DCS 0x29 (display on) (:447-449). iPad skips the PMU `0x6B/0x6C` write (:461-466). Panel-ID read failure returns -1 and quiesces (:441-445) → **emulator must answer the 0xB1 read**.
- **mipi_dsim_init** (`mipi_dsim.c:84-208`; register offsets `hardware/mipi_dsim.h:10-32`): gate 0x11 on; bypass path `CLKCTRL = ESC_PRESCALER(0x64)|ESC_CLKEN(1<<28)|PLL_BYPASS(1<<27)|BYTE_CLK_SRC(1<<25)`, `PLLCTRL = 0x04000000`, `SWRST = 1`, poll `STATUS & (1<<20)`; iPad: `MDRESOL = 768<<16|1024` (no STAND_BY, :157-161); `MVPORCH = VFP<<16|VBP|0xD<<28`, `MSYNC = VSPW<<22 | HSPW 1`, `MHPORCH = 0xF<<16|0xE`, `SDRESOL = 10`, `CONFIG = NUM_DATA(4)|EN_DATA(0xF)|MAIN_PXL_FMT(5=RGB666 for 18bpp)|HSE|AUTO|VIDEO|BURST|1` (:169), `CLKCTRL |= BYTE_CLKEN|ESC_EN_CLK|ESC_EN_DATA(0xF)`, `FIFOCTRL = 0x1F`, `FIFOTHLD = 0x1FF`, `ESCMODE = FORCE_STOP|CMD_LP` then clear FORCE_STOP, poll `STATUS & (STOP(1<<8)|DATA_STOP 0xF)`, ULPS enter/exit polls on bits 9 and 4-7 (:175-203). Command TX: `PKTHDR = id|d0<<8|d1<<16`, poll `FIFOCTRL & (1<<22)` (:31-36). Read: clear `INTSRC=0xFFFFFFFF`, send, poll `INTSRC` bit 0x40000 (done; 0x10000/0x200000 = error), wait `FIFOCTRL & 0x1000000` clear, `RXFIFO` header type `&0x3F` must be 26 (0x1A) or 28 (0x1C), length `>>8 & 0xFFFF`, then payload 4 bytes per `RXFIFO` read (:38-82).
- **Backlight (iPad-specific, SWI)** (`clcd.c:779-790`): gate 0x3E on; `0xBF600000 = ((f(TicksPerSec)&0xFF)<<8)|3`; `0xBF600018 = 0x1000|0x80|(level&0x7F)|((level&0x780)<<1)`; `0xBF600014 = 3`, poll bit0; GPIO 0x1403 = level?1:0. DT `swi`: command-vsel 6, nclk-div 0xC, command-iset 1, #command-bits 3, clock-gates 0x7C/0x7D, IRQ 7 (dt.txt:956-969). Max 2047 (`clcd.h:46`). Backlight chip part number: NOT identified (teardowns only name 338S0805; the LM3534 assumption is unsupported; the PMU regs 0x66-0x68 backlight path is `#if !CONFIG_IPAD_1G`, `clcd.c:756-777`).
- iEmu display model is a stub: `clcd_write` just sets `frame_base = 0x4F700000` and logs (`ipad1g.c:251-266`), scanout hardcoded 1024×768 at 16 bpp (`ipad1g.c:178,194-197,207-208`), mapped at `CLCD_BASE_ADDR 0x89100000` (`ipad1g.h:13`, i.e. DISPLAY_PIPE1 — wrong for iPad, which uses PIPE0, `clcd.h:9-13`).

**Minimal emulated display controller for qemu-ios must**: (1) map 0x89000000 (DISPLAY_PIPE0) and latch `+0x4044` (FB base), `+0x4048` (stride), `+0x4060` (w/h), `+0x4040` bits 8-11 (0→XRGB8888, 4→RGB565), `+0x1038` bit 8 (layer enable); (2) map 0x89200000 (CLCD) with `+0x00` bit 0x100 self-clearing, `+0x50` bit0 enable / bit1 "stopped" readback, accept LUT writes at +0x34/+0x38; (3) map 0x89500000 (DSIM) returning `STATUS = PLL_STABLE|SWRST|HS_READY|STOP|DATA_STOP(0xF)` (and ULPS bits while ESCMODE ULPS set), `FIFOCTRL` bit22 set / bit24 clear, `INTSRC` 0x40000 after a read packet, and an `RXFIFO` stream `0x1C|(len<<8)` + a panel-ID payload whose `(id>>8)&0xFF ∈ {3,4,7,8}` and `id & 0xFFFFBF == 0xB30689` so iBoot finds a gamma table; (4) map 0xBF600000 SWI with `+0x14` bit0 readable as 1; (5) treat GPIO 0x1404/0x1403/0x1301 as no-ops; (6) scan out 1024×768 from the latched base (iBoot places it at 0x4F700000; the iOS kernel will re-program it, possibly via DART2 IOVA).

## 5. Multitouch

- DT `/arm-io/spi1/multi-touch compatible 'multi-touch,k48'` on **SPI1 (0x82100000, IRQ 0x1E)**; `reg = {0, 0x7C, 0x08010000, 0x1388 (5000 kHz), 0x2710 (10000 kHz)}`; CS GPIO 0x601 (`function-spi_cs0`/`enable_cs`), reset GPIO 0x204, enable_download GPIO 0x107, ATN interrupt GPIO 0x15 (=0x205), clock_enable = PMU GPIO 0 (matches `a4.c:62`), power_ldo = PMU LDO 0x0A @ 0x0BB8 (3000 mV) (dt.txt:352-380); SPI1 DMA channels 0x12/0x13 (dt.txt:362); 1024-byte `multi-touch-calibration`, 128-byte `prox-calibration` blobs filled from syscfg. Silicon (teardowns): Broadcom BCM5973 + BCM5974 + TI CD3240A line driver.
- Protocol: no A4 driver in openiBoot; the Zephyr2/HBPP protocol is in `plat-s5l8900/multitouch-z2.c`: SPI mode CPOL=1/CPHA=1 8-bit (:1016), HBPP ATN-ACK cmd `0x1A` (:95) expecting `0xFFFF` after reset, `0x4BC1` after data OK (:183,216,257), `0x4AD1` after reg write (:331); data packets `0x18 0xE1 …` (:113-118,204-205,245-246); bootloader data packet `0x30 …` (:132); register read `0x1C` (:279) / write `0x1E` with mask (:306); calibrate `0x1F` (:378) after `writeRegister(0x10001C04, 0x16E4, 0x1FFF)` (:351); execute `0x1D … 0x18 … 0x10` (:400-404); endianness flips to little-endian after main firmware load (:439); frame header/finger structs `includes/multitouch.h:29-68`. Firmware/calibration images are supplied by iOS (`/System/Library/…`), not by iBoot (no multitouch strings in iboot.str).

## 6. Sensors

- **Accelerometer LIS331DLH** on **I2C2 addr 0x19** (7-bit) — DT dt.txt:650-659 (IRQ GPIOs 0x24, 0x26, orientation `1,0xff00,0x10000`); openiBoot `ACCEL_I2C_BUS 2`, `0x32/0x33` 8-bit, WHOAMI reg 0x0F = 0x32, CTRL_REG1 0x20 / CTRL_REG2 0x21 / OUTX/Y/Z 0x29/0x2B/0x2D (`hardware/accel.h:6-19`); init writes REG2=BOOT(0x80), REG1=PM0|XYZ(0x27) (`accel.c:47-48`).
- **ALS TSL2581** on I2C2 addr 0x39, IRQ GPIO 0x25, int_status GPIO 0x405 (dt.txt:660-671). No openiBoot A4 driver.
- Compass AKM8973S (I2C0 0x1E and I2C2 0x1E, dt.txt:619-627,681-689), prox AD7147A (I2C2 0x2C, dt.txt:690-699), ISP/camera node (dt.txt:893-907), baseband (dt.txt:1068-1085) are present in the generic k48 DT but **not populated on iPad 1** (iOS probes and fails). Mikey (headset) CD3282 on I2C0 0x39 (dt.txt:628-636).

## 7. Audio

- Codec **Cirrus CS42L61** control on **I2C0 addr 0x4A**, reset GPIO 0x106, spkr_mute GPIO 0x005, MCLK from `pwm/codec-mclk` (dt.txt:589-604,709-712); I2S0 0x84500400 (`audio-data,cs42l58`, DMA ch 0x1A/0x1B, dt.txt:713-731); I2S1 voice, I2S2 baseband. openiBoot: `audiohw-null.c` (no driver); stale `plat-s5l8900/includes/hardware/audiocodec.h:21` (WM codec bus 0) is unused.

## 8. Wi-Fi / Bluetooth (BCM4329)

- SDIO host 0x80000000 (SDHCI-compatible register map `hardware/sdio.h:10-40`), IRQ 0x26, DT clock-gate 0x30, DMA ch3 @0x80000020, `local-mac-address`, **device reset = TCA6408 expander pin 1** (dt.txt:235-249). BT on UART3 0x82800000 (`bluetooth,n88`, 3 Mbaud, bt_reset = expander pin 0, bt_wake GPIO 0x007, dt.txt:457-481).
- openiBoot `sdio.c:587-640`: clears/installs IRQ 0x26, gate 0x24, `pwr_control=0xF`, 20 MHz 4-bit, `sdio_reset()` toggles `SDIO_GPIO_DEVICE_RESET 0x6` (`sdio.h:7`, `sdio.c:153-157` — iPhone4 value, wrong for iPad), CCCR abort, enumerate. Firmware upload path exists only in `plat-s5l8900/wlan.c` (not built for A4).

## 9. Buttons, USB, NOR

- Buttons (dt.txt:1022-1034; `hardware/buttons.h:6-17`): menu/home GPIO 0x000, hold 0x001, vol-up 0x002, vol-down 0x003, ringer 0x004 (port 0 pins 0-4), active-low (`buttons.c:6-11`); wake via PMU STAT 0x180/0x181.
- USB: PHY 0x86000000 regs OPHYPWR 0/OPHYCLK 4/ORSTCON 8/0x1C/0x44/0x48/0x60 (`usbphy.h:4-13`, init `usbphy.c:7-43`); Synopsys OTG device 0x86100000 IRQ 0xD (`usb.h:18`; dt.txt:791-801, `cable-type 1`); PHY `ref-clock-sel 3` (dt.txt:777). iEmu instantiates `register_synopsys_usb(0x86100000, irq 0xd)` (`s5l8930.c:1488`).
- **NOR**: SPI0 0x82000000, CS GPIO 0x505 (dt.txt:316-328; `nor-spi/includes/hardware/nor.h:49-52` `NOR_CS 0x505, NOR_SPI 0, NOR_MAX_READ 4`), DT size **1 MiB** (`ranges 0,0,0x100000`, dt.txt:334) with partitions: diagnostic-data 0x6000+0x2000 & 0x4000+0x2000, **nvram 0xFC000+0x2000 & 0xFE000+0x2000**, raw-device (boot images) 0x8000..0xFC000 & 0x0+0x1000 (dt.txt:337-351). Protocol (`nor.c`): JEDEC 0x9F, READ 0x03, WREN 0x06, PRGM 0x02, RDSR 0x05, WRSR 0x01, EWSR 0x50, AAI 0xAD, 4 KB erase 0x20 (`nor.h:9-18`), 12 MHz (`nor.c:60`), vendors SST 0xBF/Atmel 0x1F(AT25DF081A)/ST 0x20/AMD 0x01 (`nor.c:405-461`), size hard-coded 16 MiB (`nor.c:373-376` — inconsistent with DT). iPad reads the PLog/locker (EMF/DKey) from **NOR offset 0xFA000, 0x2000 bytes**, 0x400-stride entries, magic 'kL' 0x4C6B (`h2fmi.c:2914-2946`) instead of NAND. iEmu: 1 MiB `pflash_spi` with `-pflash` image, JEDEC from `ident[]` (`s5l8930_spi.c:270-276`, `pflash_spi.c:87,120`). Actual NOR part number: NOT found.

## 10. NAND geometry (H2FMI)

- Controllers (`h2fmi.c:321-339`): fmi0 base 0x81200000 gate 0x27 IRQ 0x22 DMA ch 5(data)/6(meta); fmi1 0x81300000 gate 0x29 IRQ 0x23 DMA 7/8; 8 CE per bus, 16 total (`h2fmi.c:347-348`, `hardware/h2fmi.h:12`). DT: `fmi,s5l8920x`, gates 0x35-0x38, resets 0x27/0x29, `metadata-whitening=1`, `landing-map 0x3333/0xCCCC`, `default-ftl-version 1`, geometry fields zero until iBoot fills them (dt.txt:272-315). openiBoot iPad symmetry masks `{0,0,0,3,3,4,4,0},{0x3333,0xCCCC}` (`h2fmi.c:878-880`) match DT.
- Chip table for CONFIG_IPAD_1G (`h2fmi.c:120-130`; columns per `h2fmi.c:32-44`: blocks/CE, pages/block, bytes/page, bytes/spare, unk5, unk6, unk7, banks/CE; JEDEC maker = low byte: EC Samsung, AD Hynix, 98 Toshiba, 2C Micron):

| ReadID | blocks/CE | pages/blk | page B | spare B | banks/CE | per-CE |
|---|---|---|---|---|---|---|
| 0x7294D7EC Samsung | 0x1038 | 128 | 8192 | 0x1B4 | 1 | 4.25 GiB |
| 0x72D5DEEC Samsung | 0x2070 | 128 | 8192 | 0x1B4 | 2 | 8.5 GiB |
| 0xB614D5AD Hynix | 0x1000 | 128 | 4096 | 0x80 | 1 | 2 GiB |
| 0x2594D7AD Hynix | 0x2000 | 128 | 4096 | 0xE0 | 1 | 4 GiB |
| 0x3294E798 Toshiba | 0x1004 | 128 | 8192 | 0x1C0 | 1 | 4 GiB |
| 0x3294D798 Toshiba | 0x1034 | 128 | 8192 | 0x178 | 1 | 4 GiB |
| 0x3295DE98 Toshiba | 0x2068 | 128 | 8192 | 0x178 | 2 | 8 GiB |
| 0x3295EE98 Toshiba | 0x2008 | 128 | 8192 | 0x1C0 | 2 | 8 GiB |
| 0x4604682C Micron | 0x1000 | 256 | 4096 | 0xE0 | 1 | 4 GiB |

- Board configs (`h2fmi.c:183-200`, `{num_busses, num_symmetric, id, CEs, id2, CEs2}, vendor_type`): e.g. `{2,2,0x7294D7EC,2,+2}` = 2 buses × 2 CE = 4 CE ≈ **16 GB**; `{2,2,0x7294D7EC,4,+4}` = 8 CE ≈ **32 GB**; `{2,2,0x72D5DEEC,4,+4}` = 8 CE ≈ **64 GB**; Toshiba/Hynix/Micron equivalents listed; vendor_type 0x100014 (Samsung/Hynix), 0x150011 (Toshiba), 0x120014 (Micron). Timing rows `h2fmi.c:257-275`. Teardown confirms 16 GB = two 64 Gbit Samsung MLC packages (EDN).
- Derived geometry (`h2fmi_init`, `h2fmi.c:3066-3188`): `banks_per_ce_vfl = 1`, `bbt_format = page>>10`, `meta_per_logical_page = 0xC`, `ecc_bytes = 0xA` (`nand_some_array {0xC,0xA,0}`, `h2fmi.c:884`), ECC bits from `h2fmi_calculate_ecc_bits`, `page_size reg = (ecc_bits&0x1F)<<3|5`, whitening hash table PRNG seed 0x50F4546A (`h2fmi.c:3220-3232`; `structs.py:47-55`). VSVFL `reserved_blocks = 1` on iPad vs 16 other A4 (`vsvfl.c:1127-1131`). iBoot k48 NAND driver strings: `drivers/apple/h2fmi/H2fmi*.c`, "Chip IDs not symmetrical", "device-readid" (iboot.str:598-706).
- iphone-dataprotection: same chip table with banks_per_ce at index 7 (`nand/structs.py:20-44`); geometry from DT/plist keys `#ce, #ce-blocks, #block-pages, #page-bytes, meta-per-logical-page, vendor-type, device-readid, banks-per-ce, metadata-whitening` (`nand/nand.py:135-187`); dump page = data + meta + 8 (`nand.py:139`). iEmu image: 8-byte chip ID header, then per page `1 marker byte + page + meta` (`s5l8930_h2fmi.c:224-227,353-360,379-420`), NAND cmds via IOP coprocessor (`s5l8930_iop.c`, ARM7 VIC 0xBF300000). winocm's `danzatt/QEMU-s5l89xx-port/hw/{ipad1g,s5l8930*}` are byte-identical to iEmu (diff empty).

## 11. Not found / caveats
- No datasheet-level register map for Dialog D1815 beyond the subset openiBoot uses; no RTC code for A4.
- Backlight driver IC and NOR flash part numbers not identified; BCM4329 firmware upload for A4 not in openiBoot.
- Multitouch firmware/calibration lives in the iOS rootfs (not examined); iBoot has no MT code.
- DT geometry fields are zero (iBoot fills at runtime); exact 16/32/64 GB chip-ID per SKU inferred from board table, not from a device dump.
- DT clock-gate/clock-id numbering differs from openiBoot's gate numbers (e.g. DT mipi gate 0x69 vs openiBoot 0x11) — not reconciled.
- theiphonewiki/theapplewiki unreachable; kernelcache not decrypted/analyzed.

Sources: [EDN iPad teardown](https://www.edn.com/teardown-is-apples-ipad-a-new-computing-form-factor-or-passing-fad/), [iFixit iPad Wi-Fi Teardown](https://www.ifixit.com/Teardown/iPad+Wi-Fi+Teardown/2183), [Electronic Design – Inside the iPad](https://www.electronicdesign.com/technologies/embedded/article/21794927/inside-the-apple-ipad), [Electronics360 iPad teardown](https://electronics360.globalspec.com/article/2175/apple-ipad-16gb-wifi-mb292lla-teardown), [ipsw.me keys API](https://api.ipsw.me/v4/keys/ipsw/iPad1,1/7B367)