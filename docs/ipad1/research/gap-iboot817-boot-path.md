I have the full boot path. Compiling the report now.

# iPad 1 (K48AP / S5L8930) iOS 3.2 iBoot-817.28 — Boot-Path Spec for an Emulator

All facts from disassembly of `$SP/dec/iBoot.k48ap.bin` (172032 B, link base **0x5FF00000**, Thumb-2) and `$SP/dec/LLB.k48ap.bin` (69632 B, link base **0x84000000**) unless noted. VAs are absolute; offsets are file offset = VA − link base. `iBoot.k48ap.bin` == `fw/7B367-dec/iBoot.bin` (byte-identical, confirmed). Build string `iBoot-817.28` at 0x5FF00280.

## 0. Reset stub, relocation, entry contract

**Vector table** (iBoot 0x5FF00000, iboot.elf disasm):
- `0x5FF00000: b 0x5FF00040` (reset); other 7 vectors `ldr pc,[pc,#0x18]` from table at 0x5FF00020–0x5FF0003C = {0x5FF00040, 0x5FF12CFC, 0x5FF12D34, 0x5FF12D6C, 0x5FF12DA8, 0x5FF12DE0, 0x5FF12C6C, 0x5FF12CB4}.
- **Relocation** (0x5FF00040): `r0 = pc-72` (actual load VA); compares to `r1=[0x5FF00310]=0x5FF00000` (link base). If unequal, copies `r2=[0x5FF00314]=0x000298A0` bytes (**0x298A0 = image copy size**) from load addr to 0x5FF00000, then `bx 0x5FF00000`. **⇒ iBoot self-relocates to 0x5FF00000; an emulator may load it at any address and it will move itself, but placing it at 0x5FF00000 skips the copy.** (iboot.elf 0x5FF00044–0x5FF00070.)
- **Mode/stack setup** (0x5FF00074–0x5FF000C8): sets SP per mode from table — FIQ(0x11) SP=0x5FFF7400 [@0x320]; IRQ(0x12)/ABT(0x17)/UND(0x1B) SP=0x5FFF7800 [@0x324]; SVC(0x13) SP=0x5FFF8000 [@0x328]; clears CPSR.A (imprecise-abort mask, bic #0x100 via CPSR_x).
- **BSS clear** (0x5FF000D4): zero `[0x5FF298A0 .. 0x5FF2F000)` (`[@0x318]..[@0x31C]`).
- **C entry** (0x5FF000EC): `bx [0x5FF0030C]=0x5FF00AC9` (Thumb, `main`).

**Entry contract iBoot expects from LLB:** loose. LLB's handoff (`llb.dis` 0x84007F5C `prepare_and_jump`): quiesces clocks/UART/etc (`bl 0x84007554`, `0x840072B4`, `0x840083BC`, `0x84009008`) then `blx r6` with **r0=r1=r2=r3=0**, ARM state, MMU still on. iBoot's reset stub re-establishes its own MMU/stacks/caches from scratch, so **the only hard requirements are: privileged ARM mode, PC=iBoot base, and DRAM present/writable at the link/BSS window (0x5FF00000, 0x5FF298A0–0x5FF2F000)**. iEmu confirmed this works by entering iBoot directly with PC set (`ref-iemu.md` §4).

## 1. Memory: the 256 MiB-vs-0x5FF00000 question (RESOLVED)

- iBoot is linked at **0x5FF00000**; framebuffer at **0x5F700000** (literal x3: 0x5FF01E94, 0x5FF02254, 0x5FF0F144). DRAM is 256 MiB at 0x40000000–0x4FFFFFFF (openiBoot `a4.h:14` RAMEnd 0x50000000).
- **LLB uses framebuffer 0x4FF00000** (llb literal x3: 0x840006E8, 0x840008A0, 0x84008B34), i.e. 255 MiB above DRAM base, inside the real 256 MiB.
- **iBoot uses 0x5F700000 = 0x4FF00000 + 0x10000000.** ⇒ the SoC **physically mirrors the 256 MiB DRAM at a 0x10000000 (256 MiB) stride**: physical DRAM appears at 0x40000000 **and** 0x50000000. LLB (running from SRAM) addresses DRAM through the 0x4xxxxxxx window; iBoot relocates itself into and runs from the **0x5xxxxxxx alias** (link 0x5FF00000 → phys 0x4FF00000, fb 0x5F700000 → phys 0x4FF00000). This is a **physical mirror, not just an MMU mapping** — LLB has MMU on with its own tables yet still uses 0x4FF00000, and iBoot's fb write path stores 0x5F700000 directly into the display-pipe DMA register before its MMU is fully configured.
- **iBoot MMU tables** (`mmu_map_section` 0x5FF128F4, section attrs `0xC12 | AP/XN` per args; `mmu_setup` 0x5FF12A10). Mappings installed by `platform_init` (0x5FF105E4):
  - `0x40000000 → 0x40000000`, size **0x200 sections (512 MiB)**, cacheable (0x5FF10600).
  - `0x84000000 → 0x84000000`, size 4 MiB (SRAM identity) (0x5FF105EC).
  - `0x84000000 ← 0x84C00000`, size 4 MiB region (0x5FF10624; 0x84C00000 = openiBoot `AMC0Higher`, `a4.h:36`).
  - `0xC0000000 → 0x40000000`, size **0x400 sections (1 GiB)** — the classic high alias (0x5FF10632).
  - Base table zero-fill 0x0 size 0x1000 sections then override (0x5FF12A16).
  - Page-table base pointer stored at `[0x5FF29814]` (=0x80000000 region per `mmu_setup` literal 0x5FF12A4C).
- **RAM size determination:** I found **no runtime RAM-probe**. Sizes are compile-time constants: `gBootArgs` (0x5FF2C54C) `memSize` is written from constants during kernel prep (0x5FF0E96E stores 0xC0000000 then +0x80000000 to boot_args+4/+8 → **base 0x40000000-relative via the 0xC0000000 alias**). `vram-size` defaults **0x00900000 (9 MiB)**, floored to 0x900000 (0x5FF0DE9E `cmp #0x900000`); pram = **0x5FFFC000 size 0x4000** (0x5FF0E050/0x5FF0E058).
- **AMC/DRAM controller (0x84100000–0x84400000): NOT FOUND / not programmed.** Aligned scan finds no dense controller block; iBoot references into 0x841–0x84B are single stray literals (0x84200000, 0x84700000 ×1). LLB and iBSS likewise show no AMC init sequence. openiBoot never programs AMC (`clock.c:395-404 if(0)`, task context). **Conclusion: DRAM controller init can be skipped entirely in QEMU (RAM is always present); model the 0x40000000/0x50000000/0xC0000000 mirror instead.** iBSS (DFU first stage) is where real-hardware DRAM bring-up would live; its exact register writes were not located.

## 2. Platform init order and every peripheral base touched

**`main` (0x5FF00AC8)** call chain: `printf("\niBoot start")` (0x5FF00ACE) → **arch/cache init** 0x5FF12C0C (CP15 c1,c0,0 read-modify-write via helpers 0x5FF003E0/E8/F0/F8; clears 0x100, sets caching bits 0x1805; **no MMIO**) → **heap init** 0x5FF11800 (sets heap window `[0x5FF29764]=0x5FF2F000`, size to 0x5FF2F000..; **no MMIO**) → **platform_init** 0x5FF10800 → **clock_init** 0x5FF1138C. The boot menu/recovery logic runs later as task `0x5FF00B08`.

**First MMIO, in order** (`platform_init` 0x5FF10800):
1. **GPIO 0xBFA00000** — board-id/board-rev strap read. `gpio_configure`(0x5FF0238C), `gpio_set`(0x5FF02354), `gpio_read`(0x5FF0246C), all keyed off base **0xBFA00000** (literals @0x5FF02388/2418/2464/2498). Reads pins **0x502,0x503,0x504** (board-id), **0x202,0x301,0x304,0x305** (board-rev), assembles a word (0x5FF10956–0x5FF10990): `bits = (id2<<1)|(id3<<2)|id4 ... |(rev...<<8..16)|1`, merges high byte with `[0xBF106000]` and writes back to **0xBF106000**.
2. **0xBF106000** = **POWER_ID / board-strap register** (`r8` throughout; @0x5FF109E8). `board-id` DT prop = **`([0xBF106000] >> 16) & 0xFF`** (`get_board_id` 0x5FF10504).
3. **miu epoch check** (`miu_init` 0x5FF10ED4): `chip_power_epoch()` (0x5FF165B4 → 0x5FF10C1C reads `([0xBF500000]>>9)&0x7F`) compared to `([0xBF106000]>>24)`; mismatch → panic `"miu_init: Epoch Mismatch"` (str 0x5FF24790).

**Peripheral bases referenced (aligned literal scan, grouped):**

| Device | Base(s) | Evidence |
|---|---|---|
| **ChipID/fuses** | **0xBF500000** ×10 (0x5FF10BF8…) | §2.1 |
| **PMGR/PLL** | **0xBF100000** ×2 (0x5FF111A4/11328); clock-config array at **0xBF100040** (`prepare_and_jump` 0x5FF1141E writes `[0xBF100040]` with FSEL bits) | clock_init 0x5FF111AC |
| **Timer/tick** | **0xBF102000** ×2 (0x5FF04A3C, 0x5FF10F7C); free-run counter read at +0/+4 (`ticks` 0x5FF10F60) | 0x5FF10F00 sets +0=0x18, writes PLL-ish consts |
| **POWER_ID** | **0xBF106000** ×3 | §2.2 |
| **VIC (PL192)** | **0xBF200000** ×1 (0x5FF08410) | 0x5FF083C6 |
| **GPIO** | **0xBFA00000** ×4 | §2.1 |
| **SWI/misc** | **0xBFC00000** ×1 (0x5FF10E90) | chip helper 0x5FF10E6C |
| **PWM/backlight** | **0xBF600000** ×1 (0x5FF09A18) | 0x5FF099C4 |
| **UART0** | **0x82500000** (bank 0x82000000+n·0x100000, table 0x5FF279FC..0x5FF27C8C) | console |
| **SPI0** | **0x82000000** (NOR on SPI0) | flash_spi_* |
| **I2C0/1/2** | **0x83200000/0x83300000/0x83400000** (0x5FF290E8/29130/29178) | PMU/accel |
| **PKE (RSA)** | **0x83100000** + operand SRAM **+0x800** (0x5FF089D8/DC) | §3 |
| **SHA-1** | **0x80100000** ×1 (data 0x5FF296A8) | §3 |
| **CDMA** | ch base **0x87000000** + `chan<<12` (0x5FF01A08), global 0x87001000 | §3 |
| **CDMA-AES filter** | **0x87800000** (0x5FF019EC), 0x87801000 | §3 |
| **H2FMI0/1** | **0x81200000 / 0x81300000** (0x5FF03734…) | NAND |
| **Display pipe** | **0x89004040/4044/4050** (win DMA/fb, 0x5FF01E98…), **0x8900104C/1030/105C/205C/2064/2040/404C** (CLCD timing, 0x5FF0226C…) | §4 fb |
| **MIPI-DSIM** | **0x89200000** (0x5FF0228C, poll bit8), **0x89500000** ×3 (0x5FF0857C/8728/8960) | pinot_init |
| **NOR nvram** | via SPI0; DT `nvram reg 0x000FC000/0x2000`, raw-device `0x00008000/0xF4000` (dt.txt:342-351) | |

**ChipID 0xBF500000 bit decode** (accessor functions):
- +0 bit0 → 0x5FF10BEC (strap-valid flag)
- +0 bit1 → 0x5FF10BFC (**CPFM production bit** — `is_production` 0x5FF1FECC: if bit1 set ⇒1, else (bit0)!=0)
- +0 bits2-3 → 0x5FF10C0C (**CPRV** chip revision)
- +0 bits4-5 → 0x5FF10C4C (**GPIO epoch**)
- +0 bit7 → 0x5FF10C3C; +0 bit8 → 0x5FF10C2C
- +0 bits9-15 → 0x5FF10C1C (**power/security epoch**, feeds miu check)
- +0: `(([0xBF500000]>>9)&0x70)|(([0xBF500000]>>0xA)&7)` → 0x5FF10C64 (**SCEP composite**)
- **chip-id constant = 0x8930** (`movw r0,#0x8930`, 0x5FF10C5C) — DT `chip-id` prop.
- **ECID** = 64-bit read 0x5FF10D38 (two words from 0xBF500000+off); **die-id** 0x5FF1FEB0.
- **board-id (BDID)** = `(POWER_ID>>16)&0xFF` (0x5FF10504), **not** from chipid.

**Values that make iBoot believe it is a production K48:** miu passes when `([0xBF106000]>>24) == ([0xBF500000]>>9)&0x7F` (epochs equal). Board-id must equal the K48 expected value (compared in `prepare_and_jump`/config; **exact K48 board-id byte NOT decoded** — needs the compare constant, likely 2 for K48AP per convention, unverified). iEmu accepted chipid +0=**0x31800587** (power-epoch 2, gpio-epoch 0) and POWER_ID **0x2020001** (epoch byte 0x02) (`ref-iemu.md` §1) — a working starting point.

**The "untrusted images permitted" / skip-signature path:** governed by a **security-policy word at `[0x5FF2CFC0]`** tested by `check_flag(mask)` (0x5FF121C4: `([0x5FF2CFC0] & mask)==mask`) and `untrusted_allowed` (0x5FF121EC: `([0x5FF2CFC0] & 0x20)==0 → check_flag(0x10)`). Image3 validation failure is tolerated only when this returns true (str `"image validation failed but untrusted images are permitted"` 0x5FF23B0C, ref 0x5FF0D6C6). The DT `/chosen` flag bits mapped onto this word (`UpdateDeviceTree` 0x5FF0E0CC–0x5FF0E204): **debug-enabled=0x20**, development-cert=0x100000, production-cert=0x200000, gid-aes-key=0x40000, uid-aes-key=0x80000, **secure-boot=0x10000000**, system-trusted=0x20000000. ⇒ **to skip signature enforcement, arrange `[0x5FF2CFC0]` to have bit 0x10 set and bit 0x20 clear** (production=false / demoted). This word is initialized from fuses/CPFM early (source not fully traced — set via the production/CPFM accessors above).

## 3. Image3 load, GID/KBAG, SHA-1, PKE

- **KBAG unwrap via CDMA-AES with GID.** `cdma_init` (0x5FF019B0) gates clock 0x14. **AES filter setup** (0x5FF019C8): builds setup word `r4 = (keyselect<<1) | (encrypt&1)`, writes to **`[0x87800000]`** (`str r4,[0x87800000]`, 0x5FF019E4). The bit layout matches iEmu's model (setup bit20 custom key, **bit21 GID**, else UID; `ref-iemu.md` §1). **CDMA segment build** (0x5FF019F0): channel regs at `0x87000000 + (chan<<12)`: +0 status (write 2 = start), +4 config (`0x103` plain / `0x30103` with-data), +8 tx/rx addr, +0xC size; segment-list at `[chan*0x1000 + 0x2000_0000?]`... (segment descriptor `{next,flags,buf,size,iv[4]}`). The **GID key never leaves hardware**; on device it is used to AES-unwrap the 0x30-byte KBAG (yielding per-image key+IV) and to decrypt payloads.
- **GID oracle recommendation (viable):** all 7B367 img3 keys are public (`https://api.ipsw.me/v4/keys/ipsw/iPad1,1/7B367`). Implement a CDMA-AES model where, when the setup word selects **GID (bit21)**, a KBAG-ciphertext→(key,IV) lookup table (built from the ipsw.me keys, indexed by the 0x30-byte encrypted KBAG or by image tag/size) supplies the unwrapped key — exactly the iEmu approach generalized (iEmu keyed by plaintext size; `ref-iemu.md` §3). For a fixed build this is deterministic. GID is used **only** for KBAG unwrap and GID-tagged payloads; UID key (constant `01 23 45 67 89 AB CD EF ×2` in iEmu) handles UID-wrapped items.
- **SHA-1 engine 0x80100000: PARTIALLY FOUND.** Base confirmed by literal (0x5FF296A8) but the digest register sequence was not traced from iBoot code (the reference is in a data/driver table). iEmu maps it at 0x80100000 size 0xFF with input regs 0x40-0x7C, results 0x20-0x30 (`ref-iemu.md` §1). image3 digesting almost certainly uses it; **treat SHA-1 register semantics as unverified — reuse qemu-ios `ipod_touch_sha1.c` layout as a first cut and adjust.**
- **PKE RSA 0x83100000: FULLY TRANSFERABLE.** `pke_op` (≈0x5FF089C0): stores **`0x83100000`** as engine base and **`0x83100000 + 0x800`** as operand SRAM base (0x5FF089D8–E0), passes them plus modulus/sig segments into the Montgomery driver (0x5FF08CF8). This is the **same PKE IP and driver shape as the S5L8720** modeled in `/home/user/qemu-ios/hw/misc/ipod_touch_pke.c` / `docs/ipod-pke.md` (2 KB operand SRAM at +0x800–+0xFFF, KEY_LEN reg +0, exec +8, segment-select +0xC, precision +0x14, `A×B×R⁻¹ mod M`). **⇒ the qemu-ios PKE model and its `forge-sigcheck` approach transfer directly** (same register offsets and the final A×1 seg2→seg1 conversion the forge hook targets). This is the strongest single piece of reuse.

## 4. Kernel handoff

- **kernelcache load** (`load_kernelcache` 0x5FF0DA94): img3 tag `krnl` (0x5FF0DADC), max size **0xF00000 (15 MiB)**; compression header `comp`/`lzss` (0x5FF0DB0C/DB28); LZSS decompress to **≤0xF00000**, Adler32 check; load address from img3. Mach-O magic `feedface` checked (0x5FF0E558). Segments `__PAGEZERO`/`__PRELINK` handled (0x5FF23DC0/DD8). Kernel placed in DRAM region tracked in `gBootArgs` (0x5FF2C54C).
- **DeviceTree patch** (`UpdateDeviceTree` 0x5FF0E0BC onward): finds `/chosen`, sets policy props (§2), then `board-id`(0x5FF0E21C=get_board_id), `chip-id`(=0x8930), `unique-chip-id`(ECID 64-bit, 0x5FF0E254), `die-id`, `firmware-version`="iBoot-817.28", `display-rotation`/`display-scale` from boot_args+0x29/+0x2A, `mac-address`/`ethaddr`. Sets `/pram reg = 0x5FFFC000/0x4000` (0x5FF0E050), `/vram reg` = computed near DRAM top (base `0x3FFFC000 - poweroff_mem + memsz`, 0x5FF0E08A). `/memory`, `/chosen/memory-map` `MemoryMapReserved-0..15` (dt.txt:38-54, all currently zeroed) filled via `AllocateMemoryRange` (0x5FF0DA1A). RAMDisk node populated only if a ramdisk was loaded (0x5FF0DF10).
- **boot-args commandLine** built at buffer **0x5FF2C584** (`gBootArgs.commandLine`, printed via `"gBootArgs.commandLine = [%s]"` 0x5FF23E4C). Base string from NVRAM `boot-args`; tethered-restore path forces `"rd=md0 nand-enable-reformat=1 -progress"` (0x5FF23DE4); appends `force-usb-power=1`, `backlight-level=%d`; parses `-s`/`-v`/`debug=` to set boot flags.
- **NVRAM auto-boot semantics** (boot task 0x5FF00B08): reads `auto-boot` (0x5FF21D34); if `"true"` and no `boot-command` → `"auto-boot set but no boot-command, aborting boot"` (0x5FF21D8C). Default `boot-command` = `fsboot` (tag `recm`/`fsboot`, 0x5FF21DC0). `bootdelay` (0x5FF21E24) gives `"Delaying boot for %d seconds. Hit enter…"`. On failure/user-break → `"Entering recovery mode, starting command prompt"` (0x5FF21DD4). `fsboot` (0x5FF005EC) reads NVRAM `boot-device`/`boot-partition`/`boot-path`/`boot-ramdisk` (0x5FF21A20…), mounts HFS+ (`/System/Library/Caches/com.apple.kernelcaches/kernelcache`, 0x5FF24AF0).
- **UART0 console output** (0x82500000): banner block at boot (`"iBoot start"` 0x5FF21C39, then `=====`, `BUILD_TAG`, `BUILD_STYLE`, `USB_SERIAL_NUMBER`, `Boot Failure Count`), the CPID/CPRV/CPFM/SCEP/BDID/ECID/IBFL line (0x5FF25334), `debug-uarts` (0x5FF21C50). All via printf 0x5FF1F1EC.
- **Register state at kernel entry: NOT explicitly located** in a single jump site, but the DFU/recovery `jumping into image at 0x%08x` handler (0x5FF21FF0) and the LLB→next pattern (`blx` with r0=0) indicate the standard XNU iOS ABI: **enter with MMU off, r0 = physical address of the boot_args struct** (the `gBootArgs` at 0x5FF2C54C, physically 0x4FxxxxxX). This should be verified against the actual `prepare_and_jump` tail before implementation.

## 5. Recommended entry strategy

**Enter iBoot directly (like iEmu), do not emulate SecureROM or run LLB.** Rationale: iBoot self-relocates to 0x5FF00000 and rebuilds its own MMU/stacks; it needs no state from LLB (§0). DRAM is always present in QEMU, so the AMC init that LLB/iBSS would do is irrelevant (§1). This is exactly what iEmu did and what qemu-ios already does for the iPod (`ipod_touch_2g.c` `direct-iboot`, lines 903-969).

**Minimum device set to reach the recovery/boot menu (enter iBoot at 0x5FF00000, r0-r3=0):**
1. **DRAM** 256 MiB at 0x40000000 **mirrored at 0x50000000 and 0xC0000000** (physical alias, 0x10000000 stride). Cortex-A8.
2. **ChipID 0xBF500000** (production/epoch/CPRV/ECID/chip-id=0x8930 constants).
3. **POWER_ID 0xBF106000** (epoch high byte matching chipid; board-id byte in bits16-23).
4. **PMGR 0xBF100000** + PLL CON regs returning ENABLE|LOCKED with sane M/P/S (else UART baud div-by-zero), clock-config array at +0x40, gate regs at +0x1010 (poll bits0-3==bits4-7).
5. **Timer 0xBF102000** (free-running 24 MHz counter at +0/+4).
6. **VIC PL192 ×4 0xBF200000** stride 0x10000.
7. **GPIO 0xBFA00000** (board-id strap pins 0x502-0x504 etc. return K48 values).
8. **UART0 0x82500000** (console, S5L8720-compatible layout — reuse qemu-ios UART).
9. **security-policy word source** so `[0x5FF2CFC0]` selects the untrusted/demoted path (skip sig enforcement), **or** a working PKE+SHA1 forge.

**Additional devices to load & jump to the 3.2 kernel:**
10. **PKE 0x83100000** — reuse qemu-ios `ipod_touch_pke.c` + `forge-sigcheck` (transfers directly, §3).
11. **SHA-1 0x80100000** — adapt qemu-ios `ipod_touch_sha1.c` (layout unverified).
12. **CDMA 0x87000000 + AES filter 0x87800000** with a **GID KBAG oracle** from the ipsw.me 7B367 keys (§3).
13. **H2FMI 0x81200000/0x81300000 + NOR-SPI on SPI0 0x82000000** (fsboot reads NAND FTL / NOR nvram) — or bypass by injecting a decrypted kernelcache+DT directly into DRAM and patching the load call, exactly as qemu-ios does for the iPod (`ipod_touch_2g.c:935-969`).
14. **Display pipe 0x89000000-region + MIPI-DSIM 0x89200000/0x89500000** only for on-screen output; iBoot reaches recovery/kernel without it.

Cheapest path to first milestone (UART "iBoot start" → recovery prompt): items 1-9 only. GID/PKE/SHA/NAND (10-13) are the second milestone (kernel load); the DFU-injection bypass (13 alt) is the least-effort route to the kernel and matches existing qemu-ios tooling.

## What I could NOT find
- **AMC/DRAM controller register sequence** — no dense block in iBoot/LLB/iBSS; base unknown; not needed for QEMU (§1).
- **Exact K48 board-id compare constant** and the full init of the security-policy word `[0x5FF2CFC0]` from fuses (accessors found; the write site not traced) (§2).
- **SHA-1 0x80100000 register semantics** — base confirmed by literal only; not disassembled from a driver (§3).
- **Exact register state / boot_args physical pointer at kernel entry** — inferred from ABI and DFU jump handler, not confirmed at `prepare_and_jump` tail (§4).
- **SCEP bit-to-security-epoch mapping** beyond the composite accessor at 0x5FF10C64 (§2).
- theiphonewiki/theapplewiki unreachable; all above is from local disassembly + ipsw.me keys.