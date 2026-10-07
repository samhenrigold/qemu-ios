> Status: research, superseded by `hw/arm/ipad1.c` and `../ipad1/addresses-7B500.md`.

# iPad 1 (K48AP / iPad1,1) hardware inventory and iOS 3.2 (7B367) driver-stack expectations

Scratchpad root below = `/tmp/scratchpad`. Note: a previous step already decrypted the IPSW into `scratchpad/dec/` and `scratchpad/fw/7B367-dec/` (device tree dump `dec/dt.txt`, decompressed kernelcache `dec/kernelcache.k48.mach`, kext matching table `fw/7B367-dec/kexts.txt`, IOKit class list `fw/7B367-dec/classnames.txt`, iBoot strings `dec/iboot.str`, rootfs `fw/7B367-dec/rootfs.hfsx`, dyld cache). Most facts below come from those primary sources rather than the web, which is far more reliable than teardown press.

## 0. Identity / boot chain facts (primary sources)

| Item | Value | Source |
|---|---|---|
| Board / model / compatible | `compatible = "K48AP\0iPad1,1\0AppleARM"` (root node) | `dec/dt.txt:9` (hex decoded) |
| ApBoardID / ApChipID | 0x02 / 0x8930 (both Erase and Update identities) | `fw/7B367/BuildManifest.plist` (parsed, see BuildIdentities) |
| CPU node compatible | `"ARM,cortex-a8\0ARM,v7"`; IPI irq 0x6f | `dec/dt.txt:79-82` |
| Bootrom | SecureROM "iBoot-574.4" for s5l8930xsi; limera1n load address 0x84000000, max size 0x2C000 (vs 0x24000 on S5L8920) | https://www.theiphonewiki.com/wiki/IBoot-574.4 (search snippet); https://raw.githubusercontent.com/axi0mX/ipwndfu/master/limera1n.py |
| iBoot version | `iBoot for k48ap, Copyright 2010, Apple Inc.` / `iBoot-817.28` / `RELEASE` (same 817.28 string in LLB, iBSS, iBEC) | `dec/iboot.str`; python scan of `dec/iBoot.k48ap.bin`, `dec/LLB.k48ap.bin`, `fw/7B367-dec/iBSS.bin`, `fw/7B367-dec/iBEC.bin` |
| iBoot / iBEC link address | most-common 1 MiB bucket of absolute pointers is 0x5FF00000 (2236 hits in iBoot, 1412 in iBEC); LLB/iBSS bucket is 0x84000000 (SRAM) | same scan; matches `IBOOT_LOAD_ADDR 0x5FF00000` in `/home/user/teknogeek/iemu/hw/ipad1g.h:7` and `LLB_LOAD_ADDR 0x84000000` at `:6` |
| iBoot image sizes | iBoot 172032 B, LLB 69632 B, iBSS/iBEC 106496 B, DeviceTree 57296 B (decrypted) | `ls dec/`, `ls fw/7B367-dec/` |
| Kernel banner | `Darwin Kernel Version 10.3.1: Mon Mar 15 23:15:33 PDT 2010; root:xnu-1504.2.27~18/RELEASE_ARM_S5L8930X` | `dec/kc.strings` (strings of kernelcache.k48.mach); corroborated by a panic log quoted at https://forums.macrumors.com/threads/had-my-first-ipad-kernel-panic.890603/ |
| For comparison, qemu-ios 3.1.3 banner | `Darwin Kernel Version 10.0.0d3: ... root:xnu-1357.5.30~6/RELEASE_ARM_S5L8720X` | `/home/user/qemu-ios/hw/arm/ipod_touch_firmware.c:15` |
| Kernelcache container | IMG3 (`3gmI`, type `krnl`), payload magic `complzss`, uncompressed size 4909734 → LZSS output 9375697 B Mach-O (magic cefaedfe, cputype 12 ARM, subtype 9 = ARMv7) | python check on `fw/7B367/kernelcache.release.k48`, `dec/kc.lzss`, `dec/kernelcache.k48.mach` |
| Kernelcache build path | `/private/var/tmp/KernelCacheBuilder/KernelCacheBuilder-193.4~86/release.k48/Extensions/...`, `DTSDKName iphoneos3.2.internal`, `MinimumOSVersion 3.2` | `dec/kc.strings` (prelink info plist) |
| Rootfs | HFSX (`HX` signature, case-sensitive), blockSize 8192, 127995 blocks (≈1000 MiB, `SystemPartitionSize=1000` in `rd_options.plist`); volume name `Wildcat7B367.K48OS` | `tools/hfslist.py` output; `fw/7B367-dec/rd_options.plist` |
| fstab | `/dev/disk0s1 / hfs ro 0 1` / `/dev/disk0s2 /private/var hfs rw,nosuid,nodev 0 2` | `fw/7B367-dec/fstab` |
| Restore ramdisks | 018-7225-009.dmg (update, 10475844 B), 018-7226-009.dmg (erase, 10484036 B); OS dmg 018-7223-007.dmg | BuildManifest + `ls fw/7B367/` |
| Baseband | none on iPad1,1 Wi-Fi (ipsw.me keys list `"baseband":"06.15.00"` but DT `baseband,n82` node is a leftover, see §1) | `fw/7B367_keys.json`; `dec/dt.txt:1068-1085` |
| Codename | Wildcat | `fw/7B367_keys.json` (`"codename":"Wildcat"`); https://www.theiphonewiki.com/wiki/Wildcat_7B367_(iPad1,1) (page itself unreachable; title only) |

## 1. Hardware inventory (device tree nodes ⇄ kext matching ⇄ teardown part numbers)

### 1a. Device tree (DeviceTree.k48ap, decrypted) — full arm-io node list with physical addresses
`arm-io` `ranges = 0x00000000 → 0x80000000 size 0x40000000; 0x40000000 → 0x40000000 size 0x40000000` (`dec/dt.txt:102`), i.e. **physical = 0x80000000 + reg** for every child `reg` below 0x40000000. `arm-io/reg = 0xbfc00000 size 0x1000`, `compatible = arm-io,s5l8930x`, `device_type = s5l8930x-io`, `iommu-present`, `bus-timeout 0x7fffff` (`dec/dt.txt:99-117`).

| DT node | compatible | reg (offset → phys) | IRQ(s) | matched IOKit class (kext) | Source lines |
|---|---|---|---|---|---|
| vic | `vic,pl192` | 0x3f200000 → 0xBF200000, size 0x40000, vic-stride 0x10000 (4 VICs × 32 = 128 IRQs) | — | AppleARMPL192VIC | dt.txt:118-129; kexts.txt |
| gpio | `gpio,s5l8930x` | 0x3fa00000 → 0xBFA00000 | irq 0x74 ('t'), 6 interrupt groups, 22 ports | AppleS5L8930XGPIOIC | dt.txt:130-141 |
| pmgr (also `device_type='timer'`) | `pmgr,s5l8930x` | 0x3f100000 size 0x6000 → 0xBF100000; plus 05e00000,05f00000,08e00000,08f00000,09e00000,09f00000 (bridges) | — | AppleS5L8930XPerformanceController; `device-clocks` blob 3564 B, `clock-ids 0x118,0x119`, `mperf-dividers 6,0xa,0x1f` | dt.txt:142-160 |
| cpu-debug-interface | — | 0x3f701000 → 0xBF701000 | | | dt.txt:161-164 |
| cdma | (device_type cdma, `cdma-version 2`) | 0x07000000 size 0x26000 → 0x87000000; 0x07800000 size 0x9000 → 0x87800000 (AES engine) | irqs 0x31..(37 entries) | AppleCDMA + CDMAAES | dt.txt:165-171 |
| dart1 / dart2 | `dart,s5l8930x` | 0x08d00000 / 0x09d00000 (size 0x2000) → 0x88D00000 / 0x89D00000; mappers isp/jpeg/venc and clcd/rgbout/scaler; `invalid-translation-target 0xbf109000` | 'm' / 'n' | AppleS5L8930XDART (IODARTFamily) | dt.txt:172-234 |
| sdio | `sdio,s5l8930x`,`sdio,s5l8920x` | 0x00000000 size 0x1000 → 0x80000000 | '&' (0x26); dma ch 3 | AppleS5L8920XIOPSDIO (via IOP) → AppleBCMWLAN (K48PlatformManager, 4329ChipManager) | dt.txt:235-249 |
| sha1 | `sha1,s5l8930x`,`sha1,s5l8920x` | 0x00100000 → 0x80100000 | '%' (0x25); dma ch 4 | AppleS5L8920XSHA1 | dt.txt:250-262 |
| ceata | `ceata,s5l8930x/8920x` | 0x01000000 → 0x81000000 | '$' | (no kext match) | dt.txt:263-271 |
| flash-controller0 | `fmi,s5l8920x` | 0x01200000,0x01240000,0x01280000,0x01300000,0x01340000,0x01380000 (each 0x1000) → 0x81200000.. (H2FMI0 = 0x8120xxxx, H2FMI1 = 0x8130xxxx) | 0x22, 0x23 | AppleS5L8920XIOPFMI (runs on the IOP), child `disk` node `device_type nand`, `metadata-whitening 1`, `landing-map 0x3333 0xcccc`, `default-ftl-version 1`, `power-delay 0x02faf080` | dt.txt:272-315 |
| spi0 (+nor-flash) | `spi,s5l8930x`,`spi,s5l8900x` | 0x02000000 → 0x82000000 | 0x1d | (SPI: no explicit 8930 SPI class; DT falls back to `spi,s5l8900x` → AppleS5L8900XSPIController); `nor-flash,spi` → AppleARMSPIFlashController; children `nvram,chrp` @0xfc000/0xfe000 (2×8 KiB), `raw-device,non-nvram` (0x8000..0xfc000) → AppleImage3NORAccess, `diagnostic-data,format1` @0x6000/0x4000; ranges size 0x100000 = **1 MiB NOR** | dt.txt:316-351 |
| spi1 (+multi-touch) | same | 0x02100000 → 0x82100000; dma 0x12/0x13 | 0x1e | `multi-touch,k48` → **AppleMultitouchZ2SPI** (kext AppleMultitouchSPIZ2F13 206.5); touch irq = gpio 0x15, reset gpio 0x204, enable_cs gpio 0x601, power via PMU LDO (`function-power_ldo` → pmu phandle, 0x0bb8020a) | dt.txt:352-380 |
| spi2 | `spi,s5l8930x,baseband`,`spi,s5l8920x,baseband` | 0x02200000 → 0x82200000; dma 0x10/0x11 | 0x0c (gpio irq) | BasebandSPI (AppleS5L8920XBasebandSPIController) — unused on Wi-Fi iPad | dt.txt:381-404 |
| uart0 (`boot-console`) /iap | `uart,s5l8930x`,`uart,s5l8900x` | 0x02500000 → 0x82500000 | 0x16 | AppleS5L8900XSerial; child `iap` (dock serial) | dt.txt:405-420 |
| uart1 /debug | | 0x02600000 → 0x82600000; dma 0x0a | 0x17 | | dt.txt:421-438 |
| uart2 /umts | | 0x02700000 | 0x18 | (unused, no baseband) | dt.txt:439-456 |
| uart3 /bluetooth | `bluetooth,n88` | 0x02800000 → 0x82800000; dma 0x0c/0x0d | 0x19 | AppleBluetooth (BTReset); HCI-UART @ 0x2dc6c0 = 3 Mbaud; bt_reset via egpio | dt.txt:457-481 |
| uart5 /gas-gauge | `gas-gauge,bq27540`,`gas-gauge,hdq` | 0x02a00000 → 0x82A00000 | 0x1b | AppleHDQGasGaugeControl (TI bq27540 over HDQ on UART5) | dt.txt:482-502 |
| uart6 /gps | `gps,bcm4750` | 0x02b00000 | 0x1c | AppleBCM4750 (not populated on Wi-Fi iPad) | dt.txt:503-523 |
| pke | `pke,s5l8920x`,`pke,s5l8900x` | 0x03100000 → 0x83100000 | 0x12 | AppleS5L8900XPKE | dt.txt:524-532 |
| i2c0 | `i2c,s5l8930x`,`i2c,s5l8920x`,`iic,soft` | 0x03200000 → 0x83200000 | 0x13 | AppleS5L8920XI2CController | dt.txt:533-545 |
| i2c0/pmu | **`pmu,d1815`**, addr 0x74, irq gpio 0x0d | | | **AppleD1815PMU** 1.0.1 (+Backlight, RTC, WatchDogTimer, PowerSource, ADC, GPIO, LDO, STAT, CoreVoltage functions); properties `no-backlight`, `lcm-boost-direct-control`, `watchdog-disable`, event names gpio3=ioxpander, gpio6=battery, gpio7=land_charger, gpio9=port_charger, gpio10=mikey | dt.txt:546-574; classnames.txt |
| i2c0/egpio | `gpio-expander,tca6408`, addr 0x20, irq gpio 0x11 | | | AppleTCA6408GPIOIC; pins: gpio2=bluetooth, gpio3=wlan, gpio4=baseband, gpio5=firewire | dt.txt:575-588 |
| i2c0/audio0 | **`audio-control,cs42l61`**, addr 0x4a, reset gpio 0x106, spkr_mute gpio 0x005 | | | **AppleCS42L61Audio** 1.1; mclk from `pwm/codec-mclk` | dt.txt:589-604 |
| i2c0/charger0, i2c2/charger1 | `charger,ltc4099`, addr 0x09 | | irq via PMU | AppleLTC4099Charger | dt.txt:605-618, 672-680 |
| i2c0/compass, i2c2/compass1 | `compass,akm8973s` addr 0x1e | | | AppleAKM8973S (not populated on iPad 1; leftover) | dt.txt:619-627, 681-689 |
| i2c0/mikey | `mikey,cd3282` addr 0x39 | | | AppleCD3282Mikey (headset detect) | dt.txt:628-636 |
| i2c2 | same as i2c0 | 0x03400000 → 0x83400000 | 0x15 | | dt.txt:637-649 |
| i2c2/accelerometer | **`accelerometer,lis331dlh`** addr 0x19, irqs gpio 0x24/0x26, `orientation 1,0xff00,0x10000` | | | AppleLIS331DLH (AppleEmbeddedAccelerometer) | dt.txt:650-659 |
| i2c2/als | **`als,tsl2581`** addr 0x39, irq gpio 0x25, 5 lux curves | | | AppleTSL2581 (AppleEmbeddedLightSensor) | dt.txt:660-671 |
| i2c2/prox | `prox,ad7147a` addr 0x2c | | | AppleAD7147A (not populated; iPad has no prox) | dt.txt:690-699 |
| pwm (+codec-mclk) | `pwm,s5l8930x`,`pwm,s5l8920x` | 0x03500000 → 0x83500000 | 0x11 | AppleS5L8920XPWM | dt.txt:700-712 |
| i2s0/1/2 | `i2s,s5l8930x`,`i2s,s5l8900x` | 0x04500400 / 0x04501400 / 0x04502400 (0xc00 each) → 0x84500400.. ; dma 0x1a-0x1f | 'W','X','Y' (0x57-0x59) | AppleS5L8920XI2SController; children `audio-data,cs42l58` (i2s0), `audio-data,voice` (i2s1 → AppleSecondaryAudio), `audio-data,baseband` (i2s2) | dt.txt:713-771 |
| otgphyctrl | `otgphyctrl,s5l8930x`,`otgphyctrl,s5l8720x` | 0x06000000 → 0x86000000 | | AppleS5L8930XUSBPhy; `ref-clock-sel 3` | dt.txt:772-780 |
| usb-complex | `usb-complex,s5l8930x` | 0x3f108000 → 0xBF108000; ranges 0x06100000 size 0x600000 → 0x86100000 | | AppleS5L8930XUSBArbitrator | dt.txt:781-790 |
| usb-complex/usb-device | `usb-device,s5l8930x`,`usb-device,s5l8900x` | 0x00000000 size 0x10000 → **0x86100000** | 0x0d | **AppleSynopsysOTGDevice** (matches the s5l8900x fallback) | dt.txt:791-801 |
| usb-ehci | `usb-ehci,s5l8930x` | 0x00300000 → 0x86400000 | 0x0e | AppleUSBEHCIARM | dt.txt:802-812 |
| usb-ohci0 / usb-ohci1 | `usb-ohci,s5l8930x` / `usb-ohci1,s5l8930x` | 0x00400000 / 0x00500000 → 0x86500000 / 0x86600000 | 0x0f / 0x10 | AppleUSBOHCIARM | dt.txt:813-837 |
| iop | `iop,s5l8930x`,`iop,s5l8920x` | 0x06300000 size 0x1000 → 0x86300000; 0x3f300000 → 0xBF300000 | 0x03 | AppleS5L8920XARM7M + IOP_S5L8930X_firmware kext (118784 B; firmware section 110592 B) | dt.txt:838-849; `fw/7B367-dec/IOP_S5L8930X_firmware.kext.bin`, `iop_firmware_section.bin` |
| vxd | `vxd,s5l8930x/8920x` | 0x05000000 size 0x100000 → 0x85000000 | '0' (0x30) | AppleVXD375 2.27.0 (video decoder) | dt.txt:850-859 |
| **sgx** | `sgx,s5l8930x`,`sgx,s5l8920x` | 0x05100000 size 0x1000 → **0x85100000** | '/' (0x2f) | **SGXDriver (com.apple.IMGSGX535 v38.10, 139264 B)** | dt.txt:860-870; kexts.txt |
| venc | `venc,s5l8930x/8920x` | 0x08000000 → 0x88000000 | '.' | H2H264VideoEncoder | dt.txt:871-881 |
| jpeg | `jpeg,s5l8920x` | 0x08200000 → 0x88200000 | 's' | AppleJPEGDriver | dt.txt:882-892 |
| isp | `isp,s5l8930x` | 0x08300000 size 0xd6000, 0x08100000 | 0x08,0x09 | (no camera on iPad 1; no kext match in list) | dt.txt:893-907 |
| scaler | `scaler,s5l8930x`,`scaler,s5l8720x` | 0x09300000 → 0x89300000 | 0x0b | AppleM2ScalerCSCDriver | dt.txt:908-918 |
| **clcd** | **`clcd,s5l8930x`** | 0x09000000 size 0x7000 (display pipe 0) + 0x09200000 size 0x2000 → **0x89000000 / 0x89200000**; dma ch 0x25 @ 0x8910103c | 0x2a, 0x29 | **AppleCLCD 37.0.2** + AppleDisplayPipe 53.0.7 + IOMobileFramebuffer (IOMobileGraphicsFamily 44.0.2) | dt.txt:919-934 |
| **mipi-dsim / lcd** | `mipi-dsim,s5l8930x`,`mipi-dsim,s5l8720x`; child **`lcd,pinot`** | 0x09500000 → 0x89500000; **`#lanes = 4`** | '(' (0x28) | AppleS5L8720XMIPIDSIController → **ApplePinotLCD**; lcd reset gpio 0x1404, lcd_ldo gpio 0x1301 | dt.txt:935-955 |
| swi | `swi,s5l8930x`,`swi,s5l8720x` | 0x3f600000 → 0xBF600000 | 0x07 | (core-voltage SWI to PMU) | dt.txt:956-969 |
| rgbout | `rgbout,s5l8930x` | 0x09100000 size 0x7000 (display pipe 1) + 0x09600000 → 0x89100000 / 0x89600000 | 0x2b,0x2c | AppleRGBOUT 24.0.8 (external video path) | dt.txt:970-982 |
| tv-out | `tv-out,s5l8930x` | 0x09400000 → 0x89400000 | "'" (0x27) | AppleTVOut | dt.txt:983-995 |
| amc | `amc,s5l8920x` | 0x04100000 (0x3000), 0x04000000 (0x40000), 0x04300000 (0x5000) → 0x84100000.. | 0x56.. (23 irqs) | AppleAMCDriverManager_r2h2 (audio DMA complex) | dt.txt:996-1007 |
| displayport | `displayport,s5l8930x` | 0x04900000 size 0x2000 → 0x84900000; 768 B `calibration_data` | '-' (0x2d) | AppleSamsungDPTXController (IODisplayPortFamily: AppleSTDP3100 / ApplePS161 receivers for dock VGA/DP adapters) | dt.txt:1008-1021 |
| /buttons | `button-names = hold,menu,volup,voldown,ringerab` on gpio 1,0,2,3,4 | | | AppleM68Buttons | dt.txt:1022-1034 |
| /dock | `dock,30pin` | | | IOAccessoryManager | dt.txt:1035-1047 |
| /charger | `charger,k48`, `battery-family 3` | | | | dt.txt:1048-1067 |
| /baseband | `baseband,n82` (leftover template; no BB on iPad Wi-Fi) | | | AppleBaseband | dt.txt:1068-1085 |

Other DT facts: `chosen` carries `display-scale`, `display-rotation`, `gid-aes-key`, `uid-aes-key`, `secure-boot`, `debug-enabled`, `production-cert`, `root-matching` (256 B), `bootp-response` (300 B) placeholders (dt.txt:16-37); `chosen/memory-map` has `MemoryMapReserved-0..15` (dt.txt:38-55); `/memory`, `/pram`, `/vram` have zeroed `reg` that iBoot fills (dt.txt:87-98). No `timer` node exists separately: the timer is inside `pmgr` (`device_type='timer'`, dt.txt:145).

### 1b. Teardown part numbers (web) — cross-referenced with the DT
| Function | Part / marking | Cross-ref | Source |
|---|---|---|---|
| SoC | Apple A4, marking `N26CGM0T 1007 APL0398 33950084 YNL184A2 1004 K4X2G643GE`; 45 nm Samsung, Cortex-A8 with NEON; PoP | | https://www.ifixit.com/Teardown/iPad+Wi-Fi+Teardown/2183 ; https://www.eetimes.com/analysis-gives-first-look-inside-apples-a4-processor/ |
| DRAM | Samsung K4X2G643GE, 2 × 1 Gbit dies (2 × 128 MB) = 256 MB, PoP on A4 | | iFixit 2183; https://www.electronicdesign.com/technologies/embedded/article/21794927/inside-the-apple-ipad |
| NAND | Samsung **K9LCG08U1M** 8 GB MLC × 2 (16 GB unit); FCC-sample unit had Toshiba parts (dual source) | DT `fmi,s5l8920x`, whitening on, landing-map 0x3333/0xcccc; openiBoot iPad1G NAND ID table has 9 entries e.g. `0x7294D7EC` (Samsung), `0x3294E798`/`0x3294D798`/`0x3295DE98`/`0x3295EE98` (Toshiba 98h), `0x4604682C`, `0xB614D5AD`/`0x2594D7AD` (Hynix ADh) | iFixit 2183; https://www.ifixit.com/Teardown/iPad+FCC+Teardown/2197 ; https://www.electronicspecifier.com/news/analysis/chipworks-teardown-of-the-ipad-reveals-few-changes-in-state-of-the-art-semiconductor-technologies ; `/home/user/iDroid-Project/openiBoot/plat-a4/h2fmi.c:120-130` |
| PMU | Apple-marked **338S0805** (iFixit only says "Apple IC"); DT/kernel identify it as **Dialog D1815** (`pmu,d1815` → AppleD1815PMU). 3cparts sells 338S0805 as "power IC for iPad 1" | | iFixit 2183; http://www.3cparts.com/?goods=detail&id=9808 ; `dec/dt.txt:567` |
| Audio codec | **Cirrus CS42L61** (I2C 0x4a) per DT; I2S data node says `audio-data,cs42l58`. No teardown marking found for the codec package. | | `dec/dt.txt:599,728` |
| Touch | **Broadcom BCM5973KFBGH** (+ BCM5974 per ElectronicDesign) + **TI CD3240A1** line driver, "three-chip" solution; 30 RX × 40 TX grid, 2 × 51-pin FFC (Molex 502250-8451) | DT `multi-touch,k48` on SPI1; firmware **Z2F13,1** "Constructed Firmware" 47160 B, version `0x0146.bin`, `PreconstructedBootloadPacketType = Z2` (= Zephyr2) | iFixit 2183; electronicdesign.com; https://hackaday.io/project/177256-put-a-raspberry-pi-cm4-into-an-original-ipad/log/188901-the-big-scary-touchscreen-connectors ; `fw/7B367-dec/iPad.mtprops` |
| Display | **LG-Philips LP097X02** 9.7" 1024×768 IPS (also Samsung-sourced units), LED backlight, Novatek column drivers; panel side is LVDS | SoC side is **MIPI-DSI, 4 lanes, "pinot" panel protocol** (DT `mipi-dsim` `#lanes 4`, `lcd,pinot`; openiBoot iPad1G uses `pinot_init()` → `mipi_dsim_init()`, reset GPIO 0x1404). ⇒ a DSI→LVDS bridge must exist on the flex/board; I could not identify it. | iFixit 2197; electronicdesign.com; `dec/dt.txt:935-955`; `/home/user/iDroid-Project/openiBoot/plat-a4/clcd.c:394-401`; `plat-a4/includes/hardware/clcd.h:38` (`DISPLAYID 1` for iPad1G → DISPLAY_PIPE1 0x89100000, `CLCD_FRAMEBUFFER 0x4F700000` at `clcd.h:17`) |
| Backlight | Not PWM and not PMU-driven: PMU node has `no-backlight`; `lcm-boost-direct-control` present; iBoot has `display-backlight-calibration`/`backlight-level` strings; openiBoot A4 backlight regs LCD_BACKLIGHT_HIGH/LOW_REG 0x66/0x67 (PMU) are for iPhone 4. Exact LED driver IC on iPad 1 **not found**. | | `dec/dt.txt:547,556`; `dec/iboot.str`; `plat-a4/includes/hardware/clcd.h` |
| Wi-Fi/BT | **Broadcom BCM4329XKUBG** (802.11n + BT 2.1+EDR + FM) on module marked `X17B ES2.0-A4 / APN 339S0107 / USI 20091207`, integrated in the dock-connector flex | Wi-Fi over SDIO via IOP (`AppleS5L8920XIOPSDIO`), class `AppleBCMWLANK48PlatformManager`, `AppleBCMWLAN4329ChipManager`; BT HCI on UART3 @3 Mbaud; `wifiFirmwareLoader` launchd daemon | iFixit 2183, 2197; `classnames.txt`; `dec/dt.txt:457-481` |
| Accelerometer | STMicro **LIS331DLH** (I2C2 0x19) | Chipworks only says "STMicroelectronics accelerometer" | `dec/dt.txt:654`; electronicdesign.com |
| ALS | TAOS **TSL2581** (I2C2 0x39) | | `dec/dt.txt:665` |
| Headset detect | TI **CD3282** "mikey" (I2C0 0x39) | | `dec/dt.txt:632` |
| GPIO expander | TI **TCA6408** (I2C0 0x20) | | `dec/dt.txt:580` |
| Charger | Linear **LTC4099** (I2C 0x09) | | `dec/dt.txt:613` |
| Gas gauge | TI **bq27540** over HDQ on UART5 | | `dec/dt.txt:498` |
| NOR | 1 MiB SPI NOR on SPI0 CS0 (DT ranges size 0x100000), NVRAM at 0xFC000/0xFE000. **Part number not found** in any teardown. | | `dec/dt.txt:329-351` |
| USB | Synopsys DWC OTG device core at 0x86100000 + EHCI/OHCI hosts; PHY at 0x86000000. NXP `L061 01 4 ZSD950` on board (iFixit calls it power mgmt; likely USB/charging related, unconfirmed) | | `dec/dt.txt:772-837`; iFixit 2183 |
| Speaker amp | **Not found** (no DT node; codec drives speakers via `spkr_out`, `spkr_mute` GPIO 0x005) | | `dec/dt.txt:590-602` |
| Battery | 2 × 3.75 V Li-poly in parallel, 24.8 Wh, APN 616-0447 | | iFixit 2183, 2197 |
| Dock | 30-pin (`dock,30pin`), iAP over UART0 | | `dec/dt.txt:1035-1047, 418` |

### 1c. Kexts present in the 7B367 kernelcache that matter for emulation (with sizes)
From `fw/7B367-dec/kexts.txt` (588 lines; 889 IOKit classes in `classnames.txt`): AppleS5L8930X 36864 (GPIOIC, IO, PerformanceController←`pmgr,s5l8930x`), AppleS5L8920X 45056 (I2C, PWM, I2S, GPIO, AudioComplex), AppleS5L8900X 49152 (SPI, Timer, ClockController — the DT does not use `timer,s5l8900x`/`clkrstgen` on K48), AppleS5L8900XSerial 12288, AppleS5L8920XARM7M 32768, AppleS5L8920XIOPFMI 49152, AppleS5L8920XIOPSDIO 24576, IOP_S5L8930X_firmware 118784, AppleCDMA 20480, AppleS5L8930XDART 16384, AppleS5L8930XUSB 12288, AppleS5L8930XUSBPhy 8192, AppleSynopsysOTGDevice 32768, AppleUSBEHCI 114688, AppleUSBOHCI 57344, AppleCLCD 32768, AppleDisplayPipe 36864, IOMobileGraphicsFamily 28672, IMGSGX535 139264 (v38.10), AppleSamsungDPTX 73728, AppleRGBOUT 24576, AppleTVOut 12288, AppleM2ScalerCSCDriver 45056, AppleVXD375 98304, AppleJPEGDriver 32768, AppleAMC_r2 438272, AppleCS42L61Audio 16384, AppleD1815PMU 53248, AppleMultitouchSPI 61440 (+Z2F13/N1F55 personalities), AppleNANDFTL 196608, IOFlashStorage 40960, AppleNANDFirmware 12288, AppleImage3NORAccess 24576, AppleBCMWLAN 204800, IO80211Family 110592, AppleMobileFileIntegrity 86016 (v1.0.2), IOSurface 40960 (52.1.34), IOHIDFamily 90112 (1.6.1). Full matching table with `IOProviderClass`/`IONameMatch` per personality is in that file.

User-space graphics blobs already extracted: `fw/7B367-dec/IMGSGX535GLDriver` (682992 B, Info.plist present), `GLEngine` (1034960 B), `OpenGLES.bin`, `QuartzCore.bin`; dyld cache `fw/7B367-dec/dyld_shared_cache_armv7` (99307006 B).

## 2. iOS 3.2 vs 3.1.3 — emulator-relevant deltas

| Topic | Fact | Source |
|---|---|---|
| Kernel | xnu-1504.2.27 (Darwin 10.3.1) vs xnu-1357.5.30 (Darwin 10.0.0d3) on 3.1.3 iPod touch 2G. 3.2 is a different XNU branch (10.3 = Snow Leopard 10.6.3-era), not a point update of 3.1.x. | `dec/kc.strings`; `/home/user/qemu-ios/hw/arm/ipod_touch_firmware.c:15` |
| Architecture | Kernel and dyld cache are **armv7 only** (cputype 12 subtype 9; `dyld_v1   armv7` magic). 3.1.3/S5L8720 is armv6. Every user binary in the cache is Thumb-2/VFPv3/NEON. | python check; `head -c16 dyld_shared_cache_armv7` |
| dyld shared cache | Present: `/System/Library/Caches/com.apple.dyld/dyld_shared_cache_armv7`, 99307006 B, single arch | `tools/hfslist.py --grep com.apple.dyld` |
| Kernelcache format | IMG3 `krnl` → `complzss` (same LZSS scheme as 3.1.3), Mach-O with `__PRELINK_TEXT/_INFO/_STATE` segments (prelinked kexts) | python check; `dec/kc.strings` |
| AMFI / code signing | `com.apple.driver.AppleMobileFileIntegrity` 1.0.2 is prelinked and matches `IOResources`; launchd daemon `com.apple.MobileFileIntegrity.plist` exists; kernel has boot-arg strings `amfi_get_out_of_my_way`, `amfi_allow_any_signature`, `amfi_unrestrict_task_for_pid`, `cs_enforcement_disable` ("cs_enforcement disabled by boot-arg"), `_PE_i_can_has_debugger`; sandbox kext `com.apple.security.sandbox` and `sandboxd` present. So unlike 3.1.3-era 8720 builds, 3.2 will need either patched AMFI/cs_enforcement or boot-args if you inject unsigned binaries. | `dec/kc.strings`; `fw/7B367-dec/kexts.txt`; LaunchDaemons listing |
| NAND boot-args recognized by AppleNANDFTL | `nand-enable-whitening`, `nand-whiten-metadata`, `nand-enable-vs`, `nand-enable-yaftl`, `nand-enable-reformat`, `nand-ignore-ptab`, `nand-force-restore`, `nand-disable-driver`, `nand-fbbt-publish`, `nand-enable-readscattered/writescattered/readmultiple/writemultiple/erasemultiple`, `nand-enable-adm`, `nand-check-vs`, `nand-dump-vs-table`, `nand-neuralize`, `nand-wipe`, `nand-erase`, `nand-set-rma`, `nand-reset-burnin`, `nand-nvram-debug`, `nand-rise-ns`, `nand-fall-ns`. Restore ramdisk boot-args: `rd=md0 nand-enable-reformat=1 -progress` (from iBoot). | `dec/kc.strings`; `dec/iboot.str` |
| iBoot | iBoot-817.28 (LLB/iBSS/iBEC identical version) vs iBoot-636.x on 3.1.x 8720 devices (the 3.1.3 iPod touch 2G ships iBoot-636.66). iBoot-817 NVRAM vars seen: `auto-boot`, `boot-args`, `boot-command`, `boot-path`, `boot-ramdisk`, `diags-path`, `backlight-level`, `pinot-panel-id`, `raw-panel-id`, `lcd-panel-id`, `display-backlight-calibration`, `iBootSleepValid`; kernel path `/System/Library/Caches/com.apple.kernelcaches/kernelcache`; supports `complzss` ("unknown kernelcache compression type" otherwise); prints `CPID:%04X CPRV:%02X CPFM:%02X SCEP:%02X BDID:%02X ECID:%016llX IBFL:%02X`. Uses the WMR NAND stack (`drivers/flash_nand/OAM/iBoot/WMROAM.c`), YAFTL (`YAFTL_Open`) and VSVFL ("RESERVEDBLOCK HEADER has to be used with VSVFL formatting"; "Metadata whitening is set in NAND signature"). | `dec/iboot.str`; iBoot version table snippet at https://www.theiphonewiki.com/wiki/IBoot_(Bootloader) (search snippet; the snippet's claim "3.1.3 used iBoot-817.28~18" contradicts the decrypted binary which is 817.28 for 7B367 — trust the binary) |
| Display / UI | 1024×768 (recovery image is `recoverymode-768x1024.s5l8930x.img3`, i.e. portrait framebuffer 768 wide); SpringBoard (1366768 B binary) is orientation-aware: `SBWallpaperView`, `SBWallpaperClipView`, `ChangeDockOrientation`, `_kCAWindowServerOrientation_Landscape*`, `SBALSIntPeriodOrientation*`; iPad home screen rotates, dock restyled, wallpaper behind icons; Phone/SMS/Calculator/Clock/Weather/Stocks removed | `strings dec/SpringBoard`; `ls fw/7B367/Firmware/all_flash/...`; https://en.wikipedia.org/wiki/IPhone_OS_3 |
| launchd | `/sbin/launchd`; 63 system LaunchDaemons incl. `com.apple.SpringBoard.plist` (MachServices: `com.apple.CARenderServer`, `com.apple.iohideventsystem`, `com.apple.springboard.*`, `PurpleSystemEventPort`, `com.apple.smsserver`), `com.apple.mtmergeprops.plist` (multitouch props merge), `com.apple.wifiFirmwareLoader.plist`, `com.apple.MobileFileIntegrity.plist`, `com.apple.sandboxd.plist`, `com.apple.chud.*`, `com.apple.usbptpd`, `com.apple.iapd`, `com.apple.fairplayd`, `com.apple.lockdown`. | `tools/hfslist.py --grep LaunchDaemons/`; `fw/7B367-dec/com.apple.SpringBoard.plist` |
| USB/lockdown | `AppleUSBDeviceMux`, `AppleUSBEthernetDevice`, `AppleCDCSerialDevice`, `IOAccessoryPortUSB` (IapOverUsbHid) personalities → same usbmuxd/lockdown model as 3.1.3, so the existing libimobiledevice bridge should transfer. | `fw/7B367-dec/kexts.txt` |
| Jailbreak-era note | Spirit (May 2010) untethered 3.2 on iPad 1 via userland exploit; bootrom 574.4 is limera1n-vulnerable (DFU pwn with load addr 0x84000000 / max 0x2C000). | https://en.wikipedia.org/wiki/Blackra1n (Spirit note in snippet); https://raw.githubusercontent.com/axi0mX/ipwndfu/master/limera1n.py |

## 3. Register-map / memory-map notes beyond openiBoot

**Physical memory map (A4/K48)** — corroborated by three independent sources:
- DRAM 0x40000000–0x60000000 (256 MiB) (`/home/user/iDroid-Project/openiBoot/plat-a4/includes/hardware/a4.h:10-12` `RAMStart 0x40000000 / RAMEnd 0x60000000`; iPhone 4 512 MiB variant is `RAMEnd 0x50000000` at :14 — note openiBoot's `#if` is inverted per-device, check the guard); `LargeMemoryStart 0x46000000` (:16), `MemoryHigher 0xC0000000` (:17, DRAM alias); iemu maps RAM at both 0x40000000 and 0xC0000000 (`/home/user/teknogeek/iemu/hw/ipad1g.c:425-427`).
- SRAM 0x84000000 (LLB/iBSS link address; limera1n LOAD_ADDRESS 0x84000000, MAX_SIZE 0x2C000 → ≥176 KiB usable SRAM) (ipwndfu limera1n.py; `ipad1g.h:6`).
- iBoot/iBEC link at 0x5FF00000 (top of 256 MiB DRAM minus 1 MiB) (binary scan; `ipad1g.h:7`); openiBoot iPad1G framebuffer `CLCD_FRAMEBUFFER 0x4F700000` (`plat-a4/includes/hardware/clcd.h:17`; iemu `ipad1g.h:14`).
- Peripheral window 0x80000000–0xC0000000 (DT `arm-io/ranges`, `dec/dt.txt:102`).
- iemu constants (`/home/user/teknogeek/iemu/hw/s5l8930.h`): VIC 0xBF200000 ×4 stride 0x10000 (:20-23), PMGR 0xBF100000 (:26), timer at PMGR+0x2000 = 0xBF102000 regs TICKSLOW 0x0/TICKSHIGH 0x4/REG 0x8/0xC/TICK 0x10/0x14, IRQs 5/6 (:29-42), GPIO 0xBFA00000 irq 0x74 (:45-46), USB PHY 0x86000000 / OTG 0x86100000 irq 0xd (:49-51), MISCSYS 0xBF800000 (:55), CHIPID 0xBF500000 (:58; openiBoot `chipid.h:5`), I2C0/1/2 0x83200000/0x83300000/0x83400000 irqs 0x13/0x14/0x15 (:66-71), PMU I2C addr 0x74 (:76), charger 0x08 (:78), SPI0-4 0x82000000..0x82400000 irqs 0x1d-0x21 (:81-90), IOP irq 3 (:93), CDMA 0x87000000 + AES at +0x800000, channel IRQ base 0x30 (:107-113), SHA1 0x80100000 (:127), H2FMI0/1 0x81200000/0x81300000 irqs 0x22/0x23 (:130-133). iemu's `ipad1g` machine registers: 4×PL192, chipid, cdma+aes, sha1, timer, pmgr, gpio, uart0 (0x82500000 irq 0x16), 3×i2c with PCF50633-style PMU stub + charger stubs, 5×spi, iop (ARM7 second CPU), usb phy, ceata stub, and a dumb 1024×768 16-bpp CLCD at 0x89100000 (`/home/user/teknogeek/iemu/hw/s5l8930.c:1368-1493`, `ipad1g.c:137-292`).
- openiBoot A4: `MIPI_DSIM 0x89500000` (`plat-a4/includes/hardware/mipi_dsim.h:5`), `DISPLAY_PIPE0 0x89000000`, `DISPLAY_PIPE1 0x89100000`, `CLCD 0x89200000` (`clcd.h:4-6`), `USB_PHY 0x86000000` (`usbphy.h:4`), `PMGR0_BASE 0xBF100000` (`clock.h:9`), `H2FMI0/1_BASE` (`h2fmi.h:4-5`). iPad1G build = `plat_a4_src` + `accel.c` + null audio + `nor-spi` module, `MACH_ID=3593` (`plat-a4/iPad1G.SConscript`). iPad1G NAND timing struct `{0,0,0,3,3,4,4,0},{0x3333,0xCCCC,0}` (`plat-a4/h2fmi.c:878-880`) matches DT `soc-rise/fall-ns 3`, `nand-rise/fall-ns 4`, `landing-map` (dt.txt:288-306).
- Chip ID: `CHIPID_GET_GPIO` bits 4..5, `CHIPID_GET_POWER_EPOCH` bits 9..15 (`chipid.h:12-13`); iemu `POWER_ID 0x4000`, epoch = `>>24` (`s5l8930.h:62-63`).
- IRQ numbers from the DT (decimal in DT, hex here): uart0 0x16, uart1 0x17, spi0 0x1d, spi1 0x1e, i2c0 0x13, i2c2 0x15, otg 0x0d, ehci 0x0e, ohci 0x0f/0x10, iop 0x03, fmi 0x22/0x23, sha1 0x25, sdio 0x26, clcd 0x2a/0x29, rgbout 0x2b/0x2c, mipi 0x28, sgx 0x2f, vxd 0x30, venc 0x2e, scaler 0x0b, pwm 0x11, pke 0x12, swi 0x07, cdma 0x31.., amc 0x56.., dp 0x2d, tv-out 0x27, i2s 0x57-0x59, gpio 0x74 (all in `dec/dt.txt` at the lines listed in §1a).

## 4. Explicitly NOT found
- theiphonewiki.com / theapplewiki.com page bodies for `S5L8930`, `K48AP`, `Wildcat 7B367 (iPad1,1)`, `Bootrom 574.4` (502/403 direct; web.archive.org is blocked by the fetch tool). Only search-snippet facts (bootrom 574.4, 1 GHz, 256 MB) were obtainable.
- Any public **S5L8930 register-level documentation** beyond openiBoot/iemu (no register maps for CLCD/display-pipe, MIPI-DSIM, AMC, DART, or the IOP mailbox outside those trees).
- Exact **SPI NOR** part number; exact **backlight LED driver** IC; **speaker amplifier**; the **MIPI-DSI→LVDS bridge** chip that must sit between the A4's 4-lane DSI (`lcd,pinot`) and the LP097X02's LVDS input; the NXP `L061` function.
- Authoritative confirmation that Apple's 338S0805 = Dialog D1815 (inferred from the DT `pmu,d1815` + iFixit "Apple IC 338S0805" + parts vendors calling it "power IC"; the Cirrus CS42L61 codec's package marking on the iPad 1 board is unknown).
- A published iPad 1 device tree dump or ioreg dump online (unnecessary: we have the decrypted DT in `dec/dt.txt` / `fw/7B367-dec/DeviceTree.txt`).
- Full iBoot-636 vs iBoot-817 changelog (wiki page unreachable); only versions and the NVRAM/boot-arg strings extracted above.
- 3.1.3-vs-3.2 launchd/SpringBoard diff was not computed against the qemu-ios 3.1.3 image (no 3.1.3 rootfs listing was available in this task; 3.2 daemon list is provided for the other side of the comparison).