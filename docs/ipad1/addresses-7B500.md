# iPad1,1 firmware addresses re-derived for iOS 3.2.2 build 7B500

Target: K48AP / S5L8930 "A4". Build **7B500** (xnu-1504.2.60~1, iBoot-817.29), re-deriving every
7B367-specific (xnu-1504.2.27, iBoot-817.28) address the research reports cite.

Method: kernelcache `dec/kernelcache.mach` (__TEXT @0xc0001000; entry LC_UNIXTHREAD pc=**0xc0063040**),
`ipsw macho -n` symbol table (4597 syms — this cache DOES carry a symtab, unlike the 7B367 study copy),
prelink info parsed from `__PRELINK_INFO` (`work/kexts.txt`), and Capstone (thumbv7/armv7) via
`work/kc.py` (kernel) and `work/ib.py` (iBoot). iBoot/LLB/iBSS/iBEC raw payloads scanned aligned-32.
DeviceTree from `dec/DeviceTree.txt`. Shared cache `dyld_shared_cache_armv7` mapped manually
(3 maps: 0x30000000/0x38000000/0x343ab000). All VAs are kernel VAs / cache VAs / iBoot VAs as noted.

## Executive summary
- **Almost everything transfers.** iBoot-817.29 and the IOP EmbeddedIOP firmware are structurally
  **identical** to 817.28 (same link bases, same literals at the same VAs, same IOP vector table /
  stacks / config offsets; only the build string bumped .28→.29). The DeviceTree is effectively
  identical. The kernelcache shifted internally; every cited kernel VA was re-found.
- **Counts:** re-found & confirmed ≈ all cited items; **changed:** kernelcache internal VAs (small
  shifts), NAND kext +0x2000, IOP fw +0x2000, and — the one big one — the **shared-cache OpenGLES
  string cluster and all framework bases moved wholesale** (`AppleMBXDevice` 0x30247b5c → **0x336bdb5c**).
- **Contradictions with the research (matter most):**
  1. The research marked `mac_proc_check_get_task{,_name}` **NOT FOUND** for 7B367. In 7B500 they are
     present and pinned: **0xc01d4614** (get_task, MAC slot +0x280) and **0xc01d46ac** (get_task_name,
     slot +0x27c). (See §1.)
  2. The plan's RAM-mirror literal claim is only half-true: the aligned scan finds **0x4ff00000 ×3 in
     LLB and 0x5f700000 ×3 in iBoot, but 0x4f700000 appears 0 times** in either image. (See §6.)
  3. The `debug_enabled` global moved 0xc02787b8 → **0xc02787d8** in DATA, but is loaded into
     `PE_i_can_has_debugger` differently: the phys address is now **0x402787d8**, not 0x402787b8. The
     four AMFI boot-args ARE still `debug-enabled`-gated (confirmed by disasm). (See §1.)
  4. Every OpenGLES/shared-cache patch offset the plan hard-codes is stale by ~+0x64760x. (See §5.)

---

> **Reviewer correction (main session, 2026-09-26):** the `debug_enabled` rows below are wrong. The
> literal `PE_i_can_has_debugger` loads (c01d1702 → pool at **c01d171c**) reads **0xc02787b8** in 7B500 —
> unchanged from 7B367 — and 0xc02787d8 occurs nowhere in the kernelcache (0xc02787b8 occurs 5×:
> c000afe4, c0064d98, c01d171c, c01d191c, c023b5c4). **Phys target stays 0x402787b8.**
> Also: 0x5f700000 − 0x4ff00000 = 0x0f800000, so the LLB/iBoot framebuffer pair does *not* show a
> 256 MiB stride; the mirror remains unproven until the real-iPad probe (HW-1).
> Other __PRELINK_TEXT VAs in this file were not independently re-checked.

## 1. AMFI / code-signature enforcement

| what | 7B367 (report) | 7B500 | how verified | conf |
|---|---|---|---|---|
| Kernel entry point | — | **0xc0063040** (ARM) | LC_UNIXTHREAD pc; `kc.py c0063040 arm` = MMU/TTBR bring-up (`mcr p15,0,r5,c2,c0,0`) | high |
| kernel virt↔phys | VA 0xc0000000 ↔ phys 0x40000000 (slide 0x80000000) | **same**: VA 0xc0000000 → phys 0x40000000 | iBoot `mmu_map` installs `0xC0000000→0x40000000` size 0x400 sections (iBoot byte-identical, §6); boot_args physBase used at entry c00630c8 | high |
| `PE_i_can_has_debugger` | 0xc01d17c0 | **0xc01d1700** | symtab `_PE_i_can_has_debugger`; disasm reads global then returns it | high |
| `debug_enabled` global VA | 0xc02787b8 | **0xc02787d8** | `PE_i_can_has_debugger` c01d1702 `ldr r2,[pc]→[c01d171c]=0xc02787d8`; also memcpy dest in PE_init_platform c01d18fa `[c01d191c]=0xc02787d8` | high |
| `debug_enabled` **phys addr** | 0x402787b8 | **0x402787d8** | VA 0xc02787d8 − 0x80000000 slide | high |
| debug-enabled DT string | 0xc02144a8 | **0xc02143dc** | `find("debug-enabled\0")`; ref'd at c01d1910 (in PE_init_platform) | high |
| debug-enabled sourced from DT `/chosen` | yes (memcpy 4B) | **yes** | PE_init_platform 0xc01d1828: DTGetProperty("/chosen","debug-enabled") → memcpy(&debug_enabled, data, ≤4) at c01d18f8-c01d18fc | high |
| AMFI kext load addr | 0xc03ae000 | **0xc03ae000** (v1.0.2, size 0x16000) | prelink info `kexts.txt` | high |
| `AMFI::start` | 0xc03afe64 | **0xc03afe60** | build str "Aug 4 2010"/"22:21:37" + `AppleMobileFileIntegrity::start` refs at c03afe6a/70 | high |
| AMFI gate on PE_i_can_has_debugger | `beq` skips 4 flags | **same**: c03afea0 `ldr r3,[pc]→0xc01d1701`; c03afea2 `blx r3`; c03afea4 `cmp r0,#0`; **c03afea6 `beq 0xc03aff2e`** skips the whole boot-arg block | high |
| 4 flags gated by debug-enabled | yes (+0x64/68/6c/70) | **yes**, flag byte layout re-derived (below) | disasm of c03afeb4-c03aff2a | high |
| `amfi_unrestrict_task_for_pid` str | 0xc03b9650 | **0xc03b9484** | literal ref c03afeb4→[c03afffc] | high |
| `amfi_allow_any_signature` str | 0xc03b96a4 | **0xc03b94d8** | ref c03afed4→[c03b0004]; sets **[r5,#0x68]=1** at c03afef2 | high |
| `amfi_get_out_of_my_way` str | 0xc03b96f0 | **0xc03b9524** | ref c03afef4→[c03b000c]; sets **[r5,#0x6c]=1** at c03aff12 | high |
| `cs_enforcement_disable` str | 0xc03b9708 | **0xc03b953c** | ref c03aff14→[c03b0010]; sets **[r5,#0x70]=1** at c03aff2a | high |
| exec-time gate `AMFI_vnode_check_signature` | 0xc03b071c | **0xc03b0718** | reads singleton `*0xc03c2dc0`, `[+0x6c]` (get_out_of_my_way) bypass at c03b0726, `[+0x68]` (allow_any_signature) at c03b0740; "AMFI: Invalid signature but permitting execution" str **0xc03b9894** | high |
| AMFI singleton ptr | 0xc03c26b8 | **0xc03c2dc0** | `ldr r5,[pc]→0xc03c2dc0` in start/vnode_check | high |
| `mac_proc_check_get_task` | NOT FOUND | **0xc01d4614** (MAC policy slot **+0x280**) | iterates `mpc->mpo[+0x280]` at c01d4634/c01d4674; caller c017a864 (task_for_pid dispatcher) | med-high |
| `mac_proc_check_get_task_name` | NOT FOUND | **0xc01d46ac** (MAC policy slot **+0x27c**) | same shape reading `[+0x27c]` at c01d46d4/c01d470c; caller c017a712 | med-high |

Notes:
- The `+0x64` store at AMFI c03afe98 is `mov r3,#-1; str r3,[r5,#0x64]` (init to −1), and c03afed2 sets
  it to 1 for `amfi_unrestrict_task_for_pid`; +0x68/6c/70 init to 0 at c03afe96/9a/9c. Same semantics as
  7B367.
- `debug=` base-kernel boot-arg is still master-gated on the same `debug_enabled` global (arg parser
  c023b54c: `PE_parse_boot_argn("debug")` then `ldr [0xc02787b8→c02787d8]`), so a single write of 1 to
  phys **0x402787d8** unlocks both AMFI and `debug=`, exactly as the 7B367 plan intended (just the new
  address).
- get_task vs get_task_name discriminator = MAC-ops slot order (0x27c precedes 0x280 in
  `mac_policy_ops`; xnu-1504 orders `..._get_task_name` before `..._get_task`). Both prologues are
  `push {r4-r7,lr}`; patch either to `movs r0,#0; bx lr` for the task-port relaxation.

## 2. boot_args / CommandLine

| what | 7B367 | 7B500 | evidence | conf |
|---|---|---|---|---|
| CommandLine offset in boot_args | +0x38 | **+0x38** | `PE_boot_args`-style accessor c01d1034: `ldr r3,[PE_state+0x70]; adds r0,#0x38; bx lr` | high |
| boot_args revision check | version halfword @+2 must == 2 | **same** | pe_identify_machine c01d126c: `ldrh r3,[r0,#2]; cmp r3,#2; bne panic` (str "pe_identify_machine: Epoch Mismatch" **0xc0214314**) | high |
| PE_state global | — | **0xc02786c4** | `_PE_state` symtab; used at c01d1034/c01d182e | high |
| PE_parse_boot_argn | (0xc01d0ef1 thumb) | **0xc01d0e30** (thumb bit → 0xc01d0e31) | `_PE_parse_boot_argn` symtab | high |

Kernel version string (NANDDRIVERSIGN payload etc.): 0xc0214690 =
`"Darwin Kernel Version 10.3.1: Wed Aug  4 19:08:04 PDT 2010; root:xnu-1504.2.60~1/RELEASE_ARM_S5L8930X"`
(7B367 was `...Mon Mar 15 23:15:33 PDT 2010; root:xnu-1504.2.27~4/...`). **This string changed** — any
generator that bakes it into NANDDRIVERSIGN must use the 7B500 text (the kernel only checks nSig bytes,
so it is tolerated either way, but use the correct one).

## 3. Kext prelink load addresses (display + IOP + others)

Full table in `work/kexts.txt`. Display/GPU stack and the deltas vs the report's 7B367 values:

| kext | 7B367 | 7B500 | Δ |
|---|---|---|---|
| AppleDisplayPipe | 0xc0589000 | **0xc058a000** | +0x1000 |
| AppleCLCD | 0xc089f000 | **0xc08a1000** | +0x2000 |
| IOMobileGraphicsFamily | 0xc056e000 | **0xc056f000** | +0x1000 |
| IOSurface | 0xc0610000 | **0xc0611000** | +0x1000 |
| AppleS5L8930XDART | 0xc0592000 | **0xc0593000** | +0x1000 |
| IODARTFamily | 0xc035f000 | **0xc035f000** | 0 |
| AppleM2ScalerCSCDriver | 0xc0369000 | **0xc0369000** | 0 |
| ApplePinotLCD | 0xc0889000 | **0xc088b000** | +0x2000 |
| AppleRGBOUT | 0xc0624000 | **0xc0626000** | +0x2000 |
| AppleS5L8720X | 0xc064b000 | **0xc064d000** | +0x2000 |
| IMGSGX535 | 0xc0690000 | **0xc0692000** (v38.10, size 0x22000, KmodInfo 0xc06b36d0) | +0x2000 |

Other cited kexts: AppleARMPlatform 0xc02c8000 (same), AppleS5L8930X 0xc0642000, AppleS5L8920X
0xc0631000, AppleARMPL192VIC 0xc054c000, AppleD1815PMU 0xc065f000, AppleS5L8930XUSBPhy 0xc060f000,
AppleS5L8920XARM7M 0xc04d1000, AppleS5L8920XIOPFMI 0xc04e7000, AppleNANDFTL 0xc07ef000,
IOP_S5L8930X_firmware 0xc074c000.

### IOP firmware image — **UNCHANGED except build string** (high confidence)
Firmware blob base **0xc074e000** (7B367: 0xc074c000, +0x2000). All structural constants **identical**:

| field | 7B367 | 7B500 | evidence |
|---|---|---|---|
| link base [0x310] | 0x0 | **0x0** | fw[0x310] |
| text+data size [0x314] | 0xf160 | **0xf160** | fw[0x314] |
| bss end [0x31c] | 0x1b000 | **0x1b000** | fw[0x31c] |
| main [0x30c] | 0x7ac | **0x7ac** | fw[0x30c] |
| vector handlers @0x20 | reset0x40/undef0x8d54/swi0x8d8c/pabt0x8dc4/dabt0x8e00/resv0x8e38/irq0x8cd8/fiq0x8d14 | **all identical** | fw 0x20-0x3f |
| stacks 0x320-0x32c | 0xf960/0xfd60/0x10560/0x10d60 | **identical** | fw 0x320-0x32c |
| early hook [0x330]/[0x334] | 0xffffffff / 0x6fe8 | **identical** | fw 0x330/0x334 |
| build string @0x280 | iBoot-817.28 | **iBoot-817.29** | cstr(fw+0x280); "EmbeddedIOP for s5l8930x, Copyright 2010" @fw+0x200 |
| META key | 92a742ab08c969bf006c9412d3cc79a5 @c075b064 | **same key** @0xc075d064 | byte search |
| ConfigurationOffset/HeapBaseOffset/HeapRequired/MessageChannelSize props | present | **present** (kext strings c074d478/48c/4ac/4cc) | find() |

⇒ the entire IOP mailbox/FMI HLE spec (config @0xf018, rings, opcodes 1–12, cache/window block) from
`../research/gap-iop-mailbox-protocol.md` transfers verbatim; only relocate the blob base and expect "iBoot-817.29".

## 4. NAND kernel paths

| what | 7B367 | 7B500 | evidence | conf |
|---|---|---|---|---|
| AppleNANDFTL kext | 0xc07ee000 | **0xc07ef000** | prelink info | high |
| `AppleNANDFTL::start` | 0xc07f0a9c | **0xc07f2ac8** | prologue before nand boot-arg parse block | high |
| `WMR_Start` | 0xc07f1a64 | **0xc07f3a64** | function containing "[WMR:ERR] NAND format invalid" print (ref site 0xc07f4154) | high |
| `nand-enable-reformat` str / ref | present | str **0xc080ff98**, ref'd **0xc07f3450** | find + literal xref | high |
| `nand-wipe` str / ref | present | str **0xc080fa20**, ref'd **0xc07f3338** | " | high |
| `nand-force-restore` str / ref | present | str **0xc080fbd4**, ref'd **0xc07f3364** | " | high |
| `nand-neuralize` str | present | **0xc080fc48** | find | high |
| `nand-erase is no longer supported` | present | **0xc080f907** | find | high |
| "[WMR:ERR] NAND format invalid…" | present | **0xc081032c** | find | high |
| "YAFTL_OPEN unsupported low-level format version" | present | **0xc0814831** | find | high |
| "full NAND R/O restore" | present | **0xc08148d3** | find | high |

The boot-arg → format-flag decision logic (bit0 = format-allowed from `nand-enable-reformat`, `nand-wipe`
→ flags|=3, `nand-force-restore` → |=0x80) is intact — all three strings present and PC-literal-referenced
inside `AppleNANDFTL::start`. The offline-generator field spec in `../research/gap-nand-genesis-decision.md` §7
(DEVICEINFOBBT/NANDDRIVERSIGN/VSVFL/YaFTL layout) depends on chip geometry + `metadata-whitening`/
`default-ftl-version` DT props, both unchanged (§7), so it transfers — only rebake NANDDRIVERSIGN's
version[] with the 7B500 Darwin string (§2).

## 5. Shared cache (OpenGLES / QuartzCore) — **BASES MOVED WHOLESALE**

Framework bases (dyld image list): OpenGLES 0x30232000→**0x336a8000**; QuartzCore 0x30951000→**0x34279000**;
libGFXShared 0x3098b000; libGLImage 0x30e7d000; libGLProgrammability 0x338e5000.

| string | 7B367 VA | 7B500 VA | conf |
|---|---|---|---|
| `AppleMBXDevice` (OpenGLES loader copy) | 0x30247b5c | **0x336bdb5c** | high |
| `AppleMBXDevice` (2nd, other binary) | — | **0x332f0c68** | high |
| `IOAcceleratorES` | (0x302471xx) | **0x336bdb4c** | high |
| `com.apple.opengles` | 0x30247b04 | **0x336bdb04** | high |
| `GLEngine.bundle` | — | **0x336bdb24** | high |
| `gliInitializeLibrary` | — | **0x336bdb34** | high |
| `MBXGLEngine` | — | **0x336bdb6c** | high |
| `GLESGetEGLInterface` | — | **0x336bdb80** | high |
| `CA_ENABLE_OGL` | 0x309dc41c-ish | **0x343086dc** | high |
| `LK_ENABLE_OGL` | absent | **absent** (0 hits) — confirms 7B367 finding | high |

- The loader-string cluster keeps the **same internal byte layout** (relative offsets b04/b24/b34/b4c/
  b5c/b6c/b80 preserved) — it moved as a block by +0x64760·. So the plan's mitigation (patch the 15-byte
  `"AppleMBXDevice"` to an always-present class such as `"IOResources"`) still applies, but at
  **0x336bdb5c**, not 0x30247b5c.
- **MBX fallback path still exists** (same string set present) and both required services are still absent
  from the iPad kernelcache (no AppleMBX kext; IMGSGX535 present but SGX unresponsive without HW), so the
  GPU-less software-compositor route (`CA_ENABLE_OGL=0`, EAGLContext fails cleanly) is unchanged.
- **OpenGLES engine-loader function VA: NOT precisely re-derived.** The loader that dlopen's
  GLEngine.bundle / matches IOAcceleratorES then AppleMBXDevice lives in OpenGLES __text (base 0x336a8000);
  I confirmed the string cluster it consumes (above) but the string addresses are **not stored as 32-bit
  literals anywhere in the cache** (whole-file scan = 0) and are **not built by movw/movt** in the OpenGLES
  text window (scan = 0), so the reference mechanism (likely a shared-cache-relative fixup / `add rN,pc`
  form) needs a heavier trace than done here. Tried: literal-word search across all 3 maps; movw/movt pair
  scan 0x336a8000–0x336be000. The load-bearing patch target (the string bytes) is nailed; the function VA
  is the only display-report item left approximate.

## 6. iBoot-817.29 — **byte-layout identical to 817.28**

| what | 7B367 (817.28) | 7B500 (817.29) | evidence | conf |
|---|---|---|---|---|
| iBoot link/relocate base | 0x5FF00000 | **0x5FF00000** | [0x310]=0x5ff00000; build "iBoot-817.29" @0x5FF00280 | high |
| iBoot copy size [0x314] | 0x298A0 | **0x298A0** | raw | high |
| LLB link base | 0x84000000 | **0x84000000** | [0x310]; build 817.29 | high |
| iBSS / iBEC | 0x84000000 / 0x5FF00000 | **same** | [0x310] (iBSS 0x84000000, iBEC 0x5FF00000) | high |
| security-policy word | [0x5FF2CFC0] | **[0x5FF2CFC0]** (unchanged) | `check_flag` 0x5FF121C4: `ldr r3,[pc]→0x5ff2cfc0; and; cmp` | high |
| `check_flag(mask)` | 0x5FF121C4 | **0x5FF121C4** | disasm identical | high |
| `untrusted_allowed` | 0x5FF121EC | **0x5FF121EC** | `[0x5ff2cfc0]&0x20==0 → check_flag(0x10)` identical | high |
| policy bits (debug-enabled=0x20, secure-boot=0x10000000, prod-cert=0x200000…) | as reported | **same** (word + masks unchanged) | check_flag masks 0x20/0x10 | high |
| framebuffer literal 0x5F700000 (iBoot) | ×3 @5ff01e94/02254/0f144 | **×3 @ identical VAs** | aligned scan | high |
| LLB framebuffer 0x4FF00000 | ×3 @840006e8/008a0/08b34 | **×3 @ identical VAs** | aligned scan | high |
| "gBootArgs.commandLine = [%s]" | 0x5FF23E4C | **0x5FF23E4C** | find | high |
| tethered-restore forces "rd=md0 nand-enable-reformat=1 -progress" | 0x5FF23DE4 | **0x5FF23DE4** ("rd=md0"), "nand-enable-reformat" @0x5FF23DEB | find | high |
| MMIO base literals | see report | **all identical counts/VAs** | see below | high |

MMIO base literal counts (iBoot, aligned-32): ChipID 0xBF500000 ×10, PMGR 0xBF100000 ×2, Timer
0xBF102000 ×2, POWER_ID 0xBF106000 ×3, VIC 0xBF200000 ×1, GPIO 0xBFA00000 ×4, UART0 0x82500000 ×1,
PKE 0x83100000 ×1, SHA-1 0x80100000 ×1, CDMA-AES 0x87800000 ×1, H2FMI0 0x81200000 ×7, H2FMI1
0x81300000 ×3, MIPI-DSIM 0x89500000 ×3, DP 0x89004044 ×1 — matching the 7B367 report.

**RAM-mirror literal claim (plan-critical) — aligned 32-bit scan results:**
- LLB `0x4ff00000`: **3** (0x840006e8, 0x840008a0, 0x84008b34) ✓
- LLB `0x4f700000`: **0** ✗ (the plan lists this; it is NOT in LLB)
- iBoot `0x5f700000`: **3** (0x5ff01e94, 0x5ff02254, 0x5ff0f144) ✓
- iBoot `0x4ff00000`/`0x4f700000`: **0 / 0**
The 256 MiB physical-mirror inference (DRAM at 0x40000000 aliased at 0x50000000/0xC0000000) still holds
from the iBoot=0x5F700000 vs LLB=0x4FF00000 framebuffer pair; the specific `0x4f700000`/`0x5f700000`-
matched-pair phrasing in the plan is not supported by the bytes — only 0x4FF00000 (LLB) and 0x5F700000
(iBoot) exist.

## 7. DeviceTree diff (7B500 dump vs report's 7B367 facts)

**No material differences found.** Node-by-node against the reports:

| node | report fact | 7B500 | match |
|---|---|---|---|
| arm-io `ranges` | 0 0x80000000 0x40000000 (+ 0x40000000…) | `0 0x80000000 0x40000000  0x40000000 0x40000000 0x40000000` | ✓ |
| arm-io `iommu-present` | empty | empty | ✓ |
| clcd `reg` | 0x09000000/7000 + 0x09200000/2000 | identical | ✓ |
| clcd `interrupts` | 0x2a, 0x29 | `'*|||)'` = 0x2a … 0x29 | ✓ |
| clcd `iommu-parent`/`dma-channels` | present; ch 0x25 @0x8910103c | present; `0x25 0x8910103c 0x00080004 0x01` | ✓ |
| mipi-dsim `reg`/IRQ | 0x09500000/1000, IRQ 0x28 | 0x09500000/1000, `'('`=0x28, #lanes 4 | ✓ |
| lcd `compatible` | lcd,pinot | lcd,pinot; lcd-panel-id empty | ✓ |
| dart2 `reg` | 0x09d00000/2000 | 0x09d00000/2000 | ✓ |
| dart2 `invalid-translation-target` | 0xbf109000 | 0xbf109000 | ✓ |
| dart2 `defer-translation-clients` | 1 | 1 | ✓ |
| scaler `reg`/IRQ | 0x09300000/1000, IRQ 0xb | 0x09300000/1000, IRQ 0x0b (proper `scaler` node, not misnamed `sdio` as in the 7B367 dump) | ✓ (naming) |
| rgbout `reg` | 0x09100000/7000 + 0x09600000/1000 | identical | ✓ |
| sgx `reg`/IRQ/gates | 0x05100000/1000, IRQ 0x2f, gates 0x5d | 0x05100000/1000, `'/'`=0x2f, `']'`=0x5d, `sgx,s5l8930x\|sgx,s5l8920x` | ✓ |
| swi `reg`/IRQ | 0xbf600000, IRQ 7 | 0x3f600000/1000 (→0xbf600000), IRQ 0x07, nclk-div 0xc, str-delay 0x1f40 | ✓ |
| flash-controller `reg` | 0x81200000/0x81300000… | `0x01200000…0x01380000` (→0x812/0x813…) | ✓ |
| nand `metadata-whitening` / `default-ftl-version` | 1 / 1 | 1 / 1 | ✓ |
| iop `iop-version` | 2 | 2 | ✓ |
| /chosen policy props (debug-enabled, production-cert, secure-boot, …) | present, zeroed on prod | present, empty (iBoot fills at runtime) | ✓ |

Only cosmetic difference: the 7B500 dump prints proper node names (`scaler`, no `@unit`), where the
7B367 study noted the scaler node showed up misnamed as `sdio`. Same reg/type/IRQ, so no behavioral change.

## Not found / approximate
- **OpenGLES engine-loader function VA** (§5): string cluster pinned (0x336bdbxx), function not
  precisely traced (no 32-bit literal / movw-movt reference to the strings in the scanned windows).
- **get_task vs get_task_name exact identity** (§1): both VAs pinned (0xc01d4614 / 0xc01d46ac); which is
  which rests on MAC-ops slot order (0x280 vs 0x27c) — med-high, not proven by an independent xref.
- Everything else re-found with instruction-level evidence.
