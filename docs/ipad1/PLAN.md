# iPad 1 (K48AP / S5L8930 "A4") on iOS 3.2.2 (7B500): plan

Supersedes `~/Downloads/ipad1-ios32-feasibility/plan/plan.md` (7B367). The research reports there remain
the evidence base for *mechanisms*; every *address* in them is 7B367 and is re-derived for 7B500 in
`docs/ipad1/addresses-7B500.md`.

## What changed from the research plan

| Research plan said | Correction |
|---|---|
| Target 3.2 (7B367, xnu-1504.2.27, iBoot-817.28) | Target **3.2.2 (7B500, xnu-1504.2.60~1, iBoot-817.29)**. Same kext set (139), same mechanisms, new addresses. |
| LLB `0x4ff00000` vs iBoot `0x5f700000` proves a 256 MiB mirror | Wrong arithmetic (difference is 0x0F800000). Mirror is still likely (iBoot links at 0x5ff00000 with 256 MiB of DRAM at 0x40000000) — **measured on the real iPad in HW-1**, not assumed. |
| No `0xc0000000` alias needed | Unverified; HW-1 probes it. |
| Kernel self-format yields a mountable filesystem | It yields an **empty FTL** (no MBR, no HFS+). The base image comes from a **real 3.2.2 NAND dump** (HW-2), with a generator as the long-term path and self-format as its test oracle. |
| iBoot sig-check bypass "works the same, no new mechanism" | Unresolved (SHA1 semantics, security-policy word write site untraced). Moved off the critical path by booting the kernel directly first. |
| GLI path has 17 entry points; MBX route covers graphics | 19 entry points; MBX route is **ES 1.1 only**. ES 2.0 needs its own GLI shim. |
| M1 = iBoot console | M1 = **kernel on serial via direct kernel boot**. iBoot becomes a fidelity milestone. |
| Kernelcache LZSS "ends 14 bytes short" | Root cause was img3 decryption: the last partial AES block is encrypted into the tag padding. Fixed in `tools/img3tool.py`; 7B500 kernelcache now passes Adler-32 (9,383,936 B). |

## 7B500 vs 7B367 (from `docs/ipad1/addresses-7B500.md`)

- **iBoot-817.29 and the EmbeddedIOP firmware are structurally identical to 817.28**: same link bases, same
  MMIO literals at the same VAs, same security-policy word `[0x5FF2CFC0]`, same IOP vector table, stacks
  and config offsets. The iBoot boot-path and IOP mailbox specs transfer verbatim; the IOP blob sits at
  0xc074e000 in the kernelcache (was 0xc074c000).
- **Device tree:** no behavioural differences.
- **Kernel:** entry 0xc0063040, `boot_args` revision 2, CommandLine at +0x38. AMFI's four boot-args are
  still gated on `debug-enabled`. The `debug_enabled` global is **still VA 0xc02787b8 / phys 0x402787b8**
  (verified directly; the agent's "moved to 0x...d8" was wrong). New: `mac_proc_check_get_task{,_name}`
  found at 0xc01d4614 / 0xc01d46ac (slot-order identification, not independently confirmed).
- **NAND:** `WMR_Start` 0xc07f3a64; reformat/wipe boot-args unchanged; the NANDDRIVERSIGN version string
  changed, so any baked NAND must carry the 7B500 one.
- **Shared cache moved wholesale:** `"AppleMBXDevice"` is at **0x336bdb5c** (verified), OpenGLES base
  0x336a8000, QuartzCore 0x34279000.
- Only the kernel `debug_enabled` and the MBX string were independently re-checked by the main session;
  treat the other agent-derived VAs as "high confidence, unreviewed" until something uses them.

## Ground truth: the real iPad

Sam's iPad 1 is available with no restrictions (wipe, downgrade, jailbreak), plus a 30-pin serial cable.
The A4 bootrom has limera1n, so we always have pre-iBoot code execution. Tooling: Legacy-iOS-Kit
(limera1n, powdersn0w downgrade to 3.x, iBoot32Patcher, xpwntool).

- **HW-1 (non-destructive, ready):** `contrib/ipad1-hw/` — limera1n → signature-patched 7B500 iBSS → iBEC with two
  new console commands, `md <addr> [n]` and `mw <addr> <val>` (replacing `bgcolor` and `go`), read back over
  `irecovery -s`. Probes in `contrib/ipad1-hw/probes.txt`: ChipID, POWER_ID, PMGR PLL/clock/gate registers, timer,
  GPIO, VIC, and the DRAM mirrors. This is an interactive register oracle we can reuse whenever the
  emulator stalls on an unknown value.
- **HW-2 (destructive):** powdersn0w restore to 3.2.2, jailbreak; capture: raw NAND dump (all banks, with
  spare/meta), the device tree and boot_args as the kernel receives them, verbose kernel log over serial
  (`serial=3 -v debug=...` via patched iBEC), IORegistry dump, syslog. These become the reference logs the
  emulator is diffed against.
- **HW-3 (as needed):** SecureROM dump (limera1n allows it) so the emulator can eventually boot from ROM like
  the iPod's `bootrom_240_4`; live register reads while debugging a model; performance baselines.

## Milestones (kernel-first)

**M0 — Tooling (≈1 week).** Promote `tools/` (fixed img3tool, vfdecrypt, dscextract, hfslist, DT dump) into
`qemu-ios/imgtools/` with one self-check each. A `fetch-7b500.sh` that reproduces `7B500/dec` from the IPSW +
keys. Commit plan + research to a qemu-ios branch.

**M1 — Kernel prints on serial (≈3–4 weeks).** New `ipad1` machine: `cortex-a8`, DRAM at 0x40000000 plus
whatever mirrors HW-1 confirms, 4× PL192, PMGR timer + clocks (enough for `AppleS5L8930XPerformanceController`
and the timebase), ChipID, GPIO, UART0 (reuse), watchdog. A direct-kernel loader: map the kernelcache
Mach-O, build the flattened DT from `DeviceTree.img3` filled the way iBoot fills it (memory-map, chosen,
vram, frequencies, serials — HW-2 gives the real values), build boot_args (`-v serial=3 debug=… amfi… `),
enter at the Mach-O entry with the MMU off. Success: xnu banner → kexts matching →
"Waiting for root device". No CDMA, SHA1, PKE, H2FMI, NOR needed.

**M2 — Root filesystem mounts, launchd runs (≈4–6 weeks).** IOP HLE: `0x86300000` control regs, IOP VIC
SOFTINT doorbells, ring 0 control messages, ring 5/6 FMI commands (1,2,4,8,11 read path) against a
page-directory NAND store seeded from the HW-2 dump. CDMA: only the AES path the kernel uses (FTL keys,
data-partition key 0x89B) — not the full descriptor engine. D1815 PMU + new I2C controller (enough to not
panic). `debug-enabled` forced (DT + 7B500 kernel global) so AMFI honors its boot-args.
Success: launchd, then SpringBoard attempts, diffed against the HW-2 serial log.

**M3 — Writes and persistence (≈2–3 weeks).** FMI program/erase opcodes; overlay persistence matching the
iPod machine. Self-format (`nand-enable-reformat=1` on a blank image) as the write-path stress test.

**M4 — GPU-less home screen (≈3–4 weeks).** Display pipe 0 + CLCD + reused MIPI-DSIM; DART2 (identity first,
or drop `iommu-parent`); IOMFB swap FIFO, VBL IRQ 0x2a; SGX node removed or fails fast; `CA_ENABLE_OGL=0`.
Measure software CoreAnimation speed at 1024×768 early — if it's unusable, M6 moves up.

**M5 — Input, USB, sensors (≈3–4 weeks).** Zephyr2 multitouch on SPI1, DWC OTG + TCP-USB bridge + usbmuxd,
LIS331DLH accelerometer, bq27545 gas gauge, buttons, orientation.

**M6 — GLES 1.1 via mbxshim (≈2–3 weeks).** Patch the shared-cache `"AppleMBXDevice"` match to an
always-present class, ship mbxshim as `MBXGLEngine.bundle`, verify 3.2.2's `GLESCreateGC` offsets.

**M7 — GLES 2.0 (large, unestimated).** A 19-entry GLI engine shim with the 1652-slot dispatch, forwarded to
the host executor. Needed for ES2 iPad apps and for accelerated CoreAnimation.

**M8 — Boot-chain fidelity.** iBoot-817.29 via `direct-iboot`: full CDMA + AES/KBAG oracle, SHA1, PKE forge,
H2FMI, NOR on SPI0; boot logo, recovery mode, DFU. Later: boot from the dumped SecureROM.

**M9 — LightTouchMac device profiles.** The app assumes one machine (320×480, 128 MiB); add a per-device
profile (machine, geometry, bezel, NAND set, GL shim).

## Verification

Every milestone is checked against a real-iPad reference: serial logs (M1–M3), IORegistry dumps (M2–M5),
screenshots (M4+). `regress.py`-style harness per machine; the iPod machine's suite must stay green through
every shared-model refactor.
