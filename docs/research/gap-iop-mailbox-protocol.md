> Status: research, superseded by `hw/arm/s5l8930_iop.c` (the IOP HLE).

## 1. IOP ISA and execution model

**Verdict: ARMv4 (ARM7TDMI-class, "ARM7M" = ARM7 with long-multiply), ARM state only, no MMU, no cache-CP15, vector table at 0.**

Evidence (firmware blob `$SP/fw/7B367-dec/iop_firmware_section.bin`, disassembled to `$SP/iop/fw_arm.dis` with `llvm-objdump --triple=armv7`):
- Vector table at 0x0: `ea00000e` (b _start@0x40) + 7× `e59ff018` (ldr pc,[pc,#0x18]) with handler table at 0x20: reset=0x40, undef=0x8d54, swi=0x8d8c, pabt=0x8dc4, dabt=0x8e00, resv=0x8e38, irq=0x8cd8, fiq=0x8d14 (fw 0x00-0x3f).
- Over 0xc930 bytes of code (text ends ≈0xc930, rodata 0xc930–0xf160), instruction census: 0 movw/movt, 0 uxtb/uxth/sxtb, 0 clz, 0 ldrd/strd, 0 blx, 0 bfi/ubfx, 0 dsb/dmb/isb/wfi, 0 ldrex/strex, 0 VFP; only `umull` (6×, ARMv3M+) and `bx` (81×, ARMv4T). Thumb disassembly yields 1051 undecodable words vs 150 for ARM (all 150 in the rodata region 0xc9xx–0xf1xx). So the compiler target was armv4/armv4t, ARM state.
- CP15 accessor block at fw 0x33c–0x520 (MIDR, CTR, ID_PFR0-ID_MMFR3, SCTLR, ACTLR, DACR, TTBR, DFSR/IFSR/DFAR, TPIDR*, c15 c2 4, TLB flush, and 9 c6 "region" registers with opc2 0/1) exists but is **dead code**: the only branch into 0x330–0x520 is `bl 0x338` (mrs r0,cpsr) from 0x8ff0/0x9028/0x92ec/0x9314/0x93cc; a full scan of 32-bit words for pointers into that range finds none except 0x400 (a coincidental constant). It is iBoot's generic arch/arm/cpu code linked in unused.
- Startup (fw 0x40–0x108): `sub r0,pc,#72` (runtime base) vs link base [0x310]=**0x00000000**; if relocated, copies [0x314]=0xf160 bytes to 0 and `bx` there. Sets IRQ/FIQ/ABT/UND/SVC stacks: 0xf960 / 0xfd60 / 0x10560 / 0x10560 / 0x10d60 ([0x320..0x32c]); zeroes bss 0xf160→0x1b000 ([0x318],[0x31c]); optional early hook [0x334]=0x6fe8 is disabled by [0x330]=0xffffffff; calls main [0x30c]=0x7ac. No MMU/cache enable via CP15 anywhere; the "cache" is an external block (§2).
- Firmware kext property `PageTableBase = 0xffffffff` (no page tables) (IOP_S5L8930X_firmware.kext.bin __text c074b0f6/c074b20c, value at c074b94c).
- Abort strings "ARM %s abort in %s mode" with modes supervisor/system/undefined etc. (classic ARM mode model, not Cortex-M).
- Image: 110592 B = 0xf160 text+data, bss to 0x1b000; "EmbeddedIOP for s5l8930x, Copyright 2010" at 0x200, "RELEASE" 0x240, "iBoot-817.28" 0x280 (fw 0x200–0x2c0). It is an iBoot-tree application (task scheduler, heap, panic, "reset vector overwritten while executing task", `sys/lock.c` asserts).

**QEMU CPU model**: `arm7tdmi` (or `arm926` if you want a CP15 for convenience) will run it; `cortex-a8` also executes ARMv4 code fine but iEmu's failure was not the CPU choice (it jumped to the AP physical address instead of remapping to 0 — `s->iopenv->regs[15] = s->startaddr`, /home/user/teknogeek/iemu/hw/s5l8930_iop.c:60, and ignored the cache/window registers). Requirement that matters: PC starts at 0x0 in an IOP-private address space (§2).

## 2. Loading, address space, control block, doorbells, rings

### 2.1 AP-side load (AppleS5L8920XARM7M, `$SP/fw/7B367-dec/AppleS5L8920XARM7M.kext.bin`, Thumb-2, __text c04d1000)
- `start()` maps `reg[0]`=0x86300000 → ivar +0x60, `reg[1]`=0xBF300000 → +0x68 (mapDeviceMemoryWithIndex 0/1; strings ARM7M.cpp:150/151). DT `/arm-io/iop`: `reg = 06300000/1000, 3f300000/1000` (arm-io base 0x80000000), `interrupts=3`, `iop-version=2`, `clock-ids=0x11f`, `clock-gates='Z'`(0x5a), `function-device_reset` → pmgr ARST id 0x16 ($SP/dec/dt.txt node arm-io/iop).
- `_prepareFirmwareImage` (c04d26e0–c04d2a28): reads firmware kext props `Target=s5l8930x`, `Build=RELEASE`, `FirmwareVersion=iBoot-817.28`, `FirmwareImage` (OSData no-copy of 0x1b000 bytes at c074c000). Constant props published by IOP_S5L8930X_firmware.kext (__data c074b940..): **ConfigurationOffset=0xf018, HeapBaseOffset=0xf09c, HeapSizeOffset=0xf0a0, HeapRequired=0xa000, PageTableBase=0xffffffff, MessageChannelSize=8, PanicString=0x1a850, PanicFunction=0x1a868, PanicLog=0x12850, PanicLogBytes=PanicLogBytesMax=0x1284c** (offsets into the image; `_iopLocateFirmwareVariableWithProperty` = memSeg->ptr + prop, c04d1614–c04d1692).
- `arm7mAllocateVisibleMemory(options,size,align,&phys)` (c04d20b4): IOBufferMemoryDescriptor with options|0x10 (physically contiguous), size/alignment rounded to 4 KiB; memSeg{+8 VA, +0xc md, +0x10 iopAddr, +0x14 size}. **iopAddr = phys + 0x80000000 for iop-version 2** (c04d2158), phys + 0xC0000000 for v1 (c04d2152). For v1 the first segment must also be ≥ next-pow2(size) (c04d2196–c04d21a4, "visible memory allocation waste"). The firmware segment is allocated first (image copied in, then `_iopConfig = fw + 0xf018` (ivar +0xf4)), then a heap of HeapRequired bytes whose IOP address is written into the fw variable at 0xf09c and size at 0xf0a0 (c04d29d0–c04d2a0a), then a message buffer (8 × 128 B) whose phys goes to cfg+0x8 (c04d2f08–c04d2f30).
- **Start sequence `_arm7mStart` (c04d2418–c04d2562)**: phys = firmwareSeg->md->getPhysicalSegment(0); len rounded up to power of two ≥0x1000 (r5); if !arm7mActive: flush firmware MD (performOperation 2), provider vtable+0x360(1,0) & +0x364(1,0,0) (clock gate/power on), [+0xa0]->vtable+0x54(1);
  `REG[0x100] = 0x10`; `REQUIRE(REG[0x100] & 0x2)` else panic "ARM7M not stopped for some reason";
  `REG[0x118] = 0x86300000` (literal c04d2590);
  `REG[0x110] = phys` (firmware base);
  `REG[0x114] = size` (v2) | `(-size) & 0x3ffff000` (v1) (c04d2502–c04d2518);
  callPlatformFunction("function-device_reset") if present; enable interrupt event source; `REG[0x100] = 1` (run); arm7mActive=1; commandWakeup; watchdog setTimeoutMS(10000).
- **Stop `_arm7mStop` (c04d1b84–c04d1c24)**: if version>1 wait ≤1000×1 ms for `!(REG[0x100] & 0x4)` else panic "ARM7M MOP won't go away… need a sorcerer"; `REG[0x100]=0`; wait ≤50 polls for `REG[0x100] & 0x2` else panic "ARM7M failed to stop … Danger to Manifold!"; provider +0x364(0,0,0), +0x360(0,0).
- **Startup ping** (start(), c04d2ff2–c04d300a): after `_arm7mStart`, `sendControlMessage('nop ' , 1000 ms)`; failure → panic "IOP: startup ping failed" (ARM7M.cpp:283).
- `_arm7mSendNMI` (c04d13d4): `VIC[0x18] = 0x4` (IOP VIC0 SOFTINT bit 2 → IOP IRQ 2). `arm7mSendInterrupt` (c04d1418): `VIC[0x18] = 0x8` (SOFTINT bit 3 → IOP IRQ 3 = AP→IOP doorbell).
- Diagnostic/panic path `_diagnosticCheckGated` (c04d1e8c): invalidates firmware MD, reads `*PanicString` var (ivar +0xac → fw 0x1a850); non-NULL → "looks like the IOP has paniced"; action 1 → 'nop ' ping 1000 ms; else NMI, wait 1 s, read PanicString/PanicFunction/PanicLog/PanicLogBytes → "IOP %spanic: %.64s: %.512s".
- Control opcodes known to the kext (c04d120c–c04d1234): 'nop_','info','nmi_','slep','susp','resu','nop ','spnd','rsum' + IOKit msgs 0xe00002bc/0xe00002c2.

### 2.2 IOP-side view of the world
- Firmware links at 0; `REG[0x110]/[0x114]` is a **remap window**: IOP addresses [0, size) → AP phys [base, base+size). Not modeled by iEmu.
- DRAM alias: IOP address = AP phys + 0x80000000 (AP DRAM 0x40000000… appears at 0xC0000000…). Static translation table in fw at 0xf078: `{iop=0xC0000000, host=0x40000000, 0x40000000, size=0x20000000}` + terminator (0xf088). Lookup 0x8040–0x80b8 (16-byte entries {f0,f1,?,size}, match on f0 or f1 range), `0x80c0(host)` → seg.f0+off (host→IOP), `0x80f0` → seg[+8]+off. Every ring item and every buffer pointer in commands is passed as an **AP physical address** and translated with 0x80c0 (e.g. 0x12cc, 0x1310, 0x2324, 0x24a8, 0x253c).
- Peripherals are identity-mapped (firmware uses 0x8xxxxxxx / 0xBFxxxxxx absolute addresses, §4).

### 2.3 0x86300000 block ("MOP": IOP cache/window controller). Registers touched:
| off | who | meaning (from code) |
|---|---|---|
| 0x00 | IOP | cache control: 4=enable (0x6fb4/0x6ffc), 0=disable (0x7038, 0x70bc) |
| 0x08 | IOP | write 1 = drain/sync (0x6fd4, via 0x7968) |
| 0x0c | IOP | write 0 = clean-all (0x70c0, 0x7088) |
| 0x18 | IOP | line address for per-line op (0x7100) |
| 0x1c | IOP | write 0 = clean line (0x7104) |
| 0x20 | IOP | write 1 = invalidate-all (0x6fc0), 0 in op&2 path (0x70c8) |
| 0x24 | IOP | write 0 = invalidate line (0x7110) |
| 0x100 | AP | control/status: w 0x10 → halt/reset; bit1 (0x2) = stopped; bit2 (0x4) = busy (v2); w 1 = run; w 0 = stop |
| 0x110 | AP | firmware physical base (IOP addr 0) |
| 0x114 | AP | window size (v2) / mask (v1) |
| 0x118 | AP | written 0x86300000 (purpose unknown; likely this block's own base for the IOP side) |
Cache-op helper 0x7098(op,addr,len): op bit0=clean, bit1=invalidate, bit3=skip re-enable; len=0 → whole cache, else 16-byte lines. (iEmu ignored 0x18/0x1c/0x24 and had no 0x114/0x118.)

### 2.4 Interrupts
- IOP VIC: 4×PL192 at 0xBF300000 + n·0x10000 (init 0x6f0c: INTENCLEAR=~0, INTSELECT=0, SWPRIORITYMASK=0xffff, VECTADDR[i]=irq number; dispatcher 0x6cc0 reads IRQSTATUS then VICADDRESS (0xF00), handler table at fw 0x11180 (12 B/entry), EOI = write to 0xF00). IRQs registered: 2 = NMI (0x78b8→iop_nmi_handler), 3 = host doorbell (0x7900, clears SOFTINTCLEAR 0xBF30001C at 0x6c38, calls [0x11880]), 5 = PMGR timer1 (0x5ff0, table 0xe8a4: {0xBF10200c,0xBF102014,irq5}), 0x22/0x23 = FMI0/FMI1 (h2fmi_isr_handler, via 0x3bd4), CDMA channel IRQs (0xa4f4).
- IOP→AP: `0x6c14(irq)` writes `1<<(irq&31)` to **AP VIC SOFTINT 0xBF200018 + (irq>>5)·0x10000**; qwi doorbell = irq 3 (0x7928) → AP `interrupts=3`. So both directions are PL192 software interrupts.

### 2.5 Config block (fw 0xf018, magic 'cnfg'=0x636e6667) and "qwi" rings
Layout written by AP (`registerEndpoint`, c04d1368–c04d13b4; start() c04d2f2c): `cfg+0x8` = message-buffer phys; `cfg+0xc+8·h` = ring phys, `cfg+0x10+8·h` = ring entry count, for endpoint handle h < 8 (ARM7M_MAX_ENDPOINTS, REQUIRE line 1238). Handles (fw task table 0xf000→descriptors {name,entry,stack,ch}): **0 = "iop" control**, **1 = message**, **3 = sdio**, **5 = fmi0**, **6 = fmi1** (0xe7e8/0xe814/0xe804/0xe824).
Ring = N × 16-byte entries; word0 = item | bit0 ownership (bit1 reserved), words1–3 unused. Kext ARM7MEndpoint ivars: +0x3c dir, +0x40 size, +0x44 rxIdx(-1 empty), +0x48 txIdx(-1 full), +0x4c ring VA, +0x50 per-slot context[]. IOP channel table 0x10da4 (32 B/entry: +0 name,+4 dir,+8 size,+0xc rx,+0x10 tx,+0x14 ring,+0x18 handler,+0x1c arg; count at 0x10da0).
- send (AP c04d1478 / IOP 0xe00): `ring[tx].w0 = item | (dir^1)`; tx=(tx+1)%N; if tx==rx → tx=-1; if rx==-1 → rx=old tx; clean line; ring doorbell (IOP: 0x7928→AP IRQ3; AP: SOFTINT 8→IOP IRQ3).
- receive (AP c04d10c0 / IOP 0xd40): invalidate line; if `(w0&1)==dir`: item = w0&~3 (AP uses context[rx] instead when bit0 set); rx=(rx+1)%N; if rx==tx → rx=-1; if tx==-1 → tx=old rx.
- Directions: control: AP dir 1, IOP dir 0; message: IOP dir 1, AP dir 0 (0xaec/0x95c). Control messages are 32 B `{u32 opcode; u32 status; u32 arg…}`; IOP responds in place (status=0) and returns the same item. IOP control opcodes (0xca0–0xcb0): 'slep' (run all task sleep hooks then halt via 0x7038), 'nop ' (ping), 'rsum', 'spnd', 'ttin' (console char); unknown → "## unrecognised host opcode". Message channel: IOP sends `index*4` of a 128-byte slot in the message buffer (0xa4c–0xa60); AP `_IOPMessage` (c04d1adc) handles 'pnic' (panic "IOP panic: %.*s", len=msg[4]-8, text at msg+8) and 'tty ' (console text) then returns the slot with item 0.

## 3. IOPFMI / IOPSDIO protocol

FMI task (fw 0x1254, one per bus, ring cfg slot 5/6): receives item → `cmd = translate(item)`; invalidates 512 B (command struct is **512 bytes**); `cmd[0]` = opcode; jump table 0x1360 (opcode 1..12); unknown → "unrecognised fmi opcode", `cmd[8]=0x80000004`; then cleans 512 B and returns the item. `cmd[8]` = status; kext reads it in `_fmiTranslateResult` (c04e7ef0, literals c04e8018–c04e8068):
1=success(0), 2=kIOReturnUnformattedMedia (blank), 0x80000001="device error"→kIOReturnBadMedia, 0x80000002→panic "IOP device timeout", 0x80000003→"IOP DMA timeout", 0x80000004→"IOP claims invalid parameter", 0x80000005→"IOP refused opcode 0x%02lx as unimplemented", 0x80000011→"IOP failed to reset all", 0x80000012→"IOP failed to read ID", 0x80000013/0x80000015→special path c04e8010, 0x80000014/0x8000001b/0x80000023–25→BadMedia, 0x8000001c/1d/1f→kIOReturnTimeout, 0x8000001e→"IOP failed ECC cleanup"; else `(status&0xffff0000)==0x80210000` flag test.

| op | handler | name (string xref) | command fields (offsets in 512 B struct) |
|---|---|---|---|
| 1 | 0x2628 | h2fmi_iop_set_config | +0x10 bus(0→0x81200000,else 0x81300000), +0x14 num_of_ce, +0x18 valid_ces bitmap, +0x1c pages_per_block, +0x24 bytes_per_page, +0x28 bytes_per_spare, +0x2c blocks_per_ce, +0x30 read_sample, +0x34 read_setup, +0x38 read_hold, +0x3c write_setup, +0x40 write_hold cycles → if_ctrl=(rs&15)<<16|(rsetup&15)<<12|(rhold&15)<<8|(wsetup&15)<<4|(whold&15); banks_per_ce=1, bytes_per_meta=10 hard-coded |
| 2 | 0x2520 | h2fmi_iop_reset_everything | +0x10 = phys of 80-byte ID buffer (16 CE × 5 B); does FMC_ON=1 (base+0x40000), IF_CTRL (base+0x40008), h2fmi_nand_reset_all (0x3a24) else 0x80000011, read_id per CE (0x2d5c) else 0x80000012; cleans buffer |
| 3 | 0x240c | erase single (calls 0x2f08=h2fmi_erase_blocks, count 1) | +0x10 ce, +0x14 block; out +0x18, +0x20 |
| 4 | 0x22ac | h2fmi_iop_read_single | +0x10 ce, +0x14 page, +0x18 data buf, +0x1c meta buf, +0x20 corrections buf, +0x24 AES struct (translated), +0x28/+0x2c AES params; invalidates data (fmi[0xc]·4 bytes… sectors) |
| 5 | 0x1be4 | raw page read (0x474c; h2fmi_read_raw_page) | +0x10 ce, +0x14 page, +0x18 buf |
| 6 | 0x1ea4 | h2fmi_iop_write_single | as 4 |
| 7 | 0x1b88 | raw page write (0x55b8; h2fmi_write_raw_page) | +0x10, +0x14, +0x18 buf |
| 8 | 0x1f94 | h2fmi_iop_read_multiple | +0x10 count, +0x14 CE array ptr, +0x18 page array ptr, +0x2c, +0x68 data segments ptr, +0x6c … (all AP phys, translated) |
| 9 | 0x1c30 | h2fmi_iop_write_multiple | same shape as 8 |
| 10 | 0x1a28 | h2fmi_iop_write_bootpage | |
| 11 | 0x1ab8 | h2fmi_iop_read_bootpage | |
| 12 | 0x2484 | erase multiple | +0x10 count, +0x14.. 16 CEs, +0x54.. 16 blocks, +0x94 out, +0xa0 status-array ptr, +0xa4 its length |
Kext sends commands from a 512-byte-stride pool (`_fmiSendCommand` c04e7de8: VA=pool+idx·0x200, phys=physpool+idx·0x200 → endpoint send). Kext also honors `#max-read-pages/#max-write-pages`, `nand-enable-{readmultiple,writemultiple,readscattered,writescattered,whitening,erasemultiple,vs,adm}` and the DT `flash-controller0/disk` timing props (soc/nand rise/fall 3/4 ns) when computing set_config cycles. Exact kext call sites that fill `cmd[0]` were **not** located (opcode written inside AppleS5L8920XIOPFMIDMACommand; not pinned).

SDIO task (fw 0x1524, cfg slot 3): 512-byte command, `cmd[0]` opcode 1..7, `cmd[8]` status (0 ok, 2 = "unrecognised sdio opcode"), args at +0xc, results at +0x14 (0x15d4–0x1634). Jump table 0x15b8: 1→0x1638 **ping** ("SDHC: Ping received, IOKit <--> IOP Connection established"), 2→0x184c, 3→0x1818 (frees a buffer, 0x717c), 4→0x17b8 (init: 0x617c/0x68f0/0x69d0), 5→0x1720 (reset/clock: 0x624c/0x609c/0x6058), 6→0x1700, 7→0x16b0 → 0x6564 (data transfer; returns 0x401 if arg[0]==0; params {cmd, arg, blkSize, blkCount, segments…} at +0x14/+0x18/+0x28/+0x34/+0x38). Names for 2–7 are inferred, not proven.

## 4. AP peripherals the IOP firmware touches
- FMI0/FMI1 0x81200000/0x81300000: FMI regs at +0x0 (CONFIG/CTRL/STAT/INTPND/INT_EN/DBG0-3/DATSIZ per dump string), FMC at +0x40000 (ON, IFCTRL@+8, CECTRL, RWCTRL, CMD, ADDR0/1, STATUS, RBB_CFG), ECC at +0x80000 (PND/MASK) — matches DT `reg` (0x01200000/0x01240000/0x01280000, 0x0130…) and iEmu s5l8930_h2fmi.c:238–270 offsets. FMI IRQs 0x22/0x23.
- CDMA 0x87000000 + ch·0x1000 (regs +0x10…+0x3c, 0x9990–0x9a60) and 0x87800000 + ch·0x1000 (per-channel AES/status block, 0x9b94–0x9c3c); DT cdma reg 0x07000000/0x26000 + 0x07800000/0x9000, cdma-version 2. FIFO table fw 0xe8cc: {3,4,0,0,0x80000020} SDIO, {3,4,0,1,0x800000a0/a4}, {5,8,1,0,0x81000020}, {5,8,1,1..4, 0x81200014, 0x81200018, 0x81300014, 0x81300018} (FMI0/1 data/meta FIFOs). FMI uses CDMA channels 5–8 (iEmu s5l8930.h:110–113, openiBoot dma0/dma1); channel IRQs 0x30+ch.
- PMGR 0xBF100000: clock-gate registers 0xBF101010 + n·4 (bit31 toggled, 0x79b8–0x79dc; gate enable via 0xa740 for FMI bus); **timer 0xBF102000**: 64-bit free-running counter (+0/+4, 0x5e98), 24 MHz (0x5edc), timer channels {0xBF102008,0xBF102010,irq6}/{0xBF10200c,0xBF102014,irq5} (table 0xe8a4).
- SDHC 0x80000000 (DT sdio reg 0/0x1000, dma ch 3 fifo 0x80000020, clock 0x129).
- Own VIC 0xBF300000–0xBF33FFFF; AP VIC SOFTINT 0xBF200018 (IRQ 3).
- 0x86300000 cache/window block (§2.3).
- SHA1/AES engines are driven through CDMA (0x87800000 block); no direct AES register writes found outside that.

## 5. Recommendation

**Option (a) second CPU running the real firmware** — needs: `arm7tdmi` CPU with private AddressSpace: window [0, 2^n) → RAM alias at REG[0x110] (size REG[0x114]), DRAM alias at 0xC0000000, MMIO shared with AP; 0x86300000 device (AP regs 0x100/0x110/0x114/0x118 with stop/run/busy bits, IOP cache ops as no-ops); 4 chained PL192 at 0xBF300000 with working SOFTINT/SOFTINTCLEAR/VECTADDR/VICADDRESS (and SOFTINT on the AP's VIC for IRQ 3); PMGR timer 0xBF102000 (counter + timer1 IRQ 5); PMGR clock gates 0xBF101010+; full H2FMI (FMI/FMC/ECC three blocks, 8-bit NAND command sequencer, ECC status semantics, 2 buses), CDMA (channels 3,5–8 with FIFO/segment lists, AES pass-through for `aes_iv_array`), SDHC. The firmware is only 110 KB and 12+7 opcodes, but the hardware-facing driver code (h2fmi_*_ISR_state_machine, CDMA, ECC) is the part nobody has cycle-accurate docs for; cmw/winocm ran aground here. Estimate: 3–5 engineer-months, high risk (H2FMI+CDMA fidelity), payoff is NAND behavior identical to hardware.

**Option (b) HLE the mailbox** — recommended. Implement `s5l8930-iop` device that: models REG[0x100] (return bit1 after 0x10/0 writes, clear bit2), stores 0x110/0x114/0x118; on `REG[0x100]=1` parses cfg at base+0xf018 (rings at +0xc+8h, msg buffer +0x8), sets fw vars 0x1a850 (PanicString)=0; on IOP-VIC SOFTINT bit3 walks ring 0 (control: answer 'nop ','slep','spnd','rsum' with status 0 and flip bit0), ring 5/6 (FMI: 512-B command, opcodes 1,2,4,5,6,7,8,9,10,11,3,12 against a NAND model addressed by (bus, ce, page), writing status 1 / 2(blank) / 0x80000001, ID bytes for op 2, page/meta/corrections buffers for 4/8 at AP phys addresses), ring 3 (SDIO: op 1 ping, 4/5 init, 6 cmd, 7 transfer → forward to an SDHCI/BCM4329 model); then raise AP IRQ 3 by setting SOFTINT bit 3 on AP VIC0. Minimal boot-to-rootfs set: control 'nop '; FMI 1 (set_config), 2 (reset+IDs), 4 (read_single), 8 (read_multiple), 11 (read_bootpage) — writes (6, 9, 3, 12, 10) only for VFL/FTL journaling (iOS 3.2 mounts rootfs RO, but the FTL/NVRAM writes happen; return success and update the image). Wi-Fi: SDIO 1, 4, 5, 6, 7. Nothing needs the IOP's cache ops, timer, CDMA or PMGR. Estimate: 2–4 engineer-weeks for the mailbox + FMI HLE on top of the existing synthetic-NAND/VSVFL/YAFTL tooling (python_scripts/nand from iphone-dataprotection gives page/meta formats), plus 2–3 weeks for SDIO/BCM4329 if Wi-Fi is required.

## 6. Not determined
- Meaning of `REG[0x118]` and whether the 0x114 "size" is a mask or a size for v2 (kext writes plain size).
- Kext-side call sites writing IOPFMI `cmd[0]` (inside AppleS5L8920XIOPFMIDMACommand); exact multi-page command sub-fields beyond the offsets listed; VS/ADM ("nand-enable-vs") table format.
- SDIO opcode 2–7 authoritative names; SDHC register-level behavior the IOP expects.
- Whether the IOP's 0x800000a0/a4 FIFO entries are SHA or AES (DT places SHA1 at 0x80100000).
- Provider vtable slots +0x360/+0x364 (assumed clock-gate enable/disable) and `[this+0xa0]->+0x54`.
- WebSearch corroboration from the iBoot source leak (`apps/EmbeddedIOP`, `arch/arm` ARM_CPU for the IOP build) was not performed; conclusion rests on binary evidence above.