# N45 ROM entry and retained-RAM wake investigation

`x-rom-boot=on` is an experimental machine option, default off. It enters the
ARM reset vector at zero, where a read-only alias exposes the supplied 64 KiB
ROM also mapped at 0x20000000. This mode does not stage iBoot, substitute ROM
jump-table slots, extract an epoch from guest instructions, or expose the
synthetic 8900-image-decrypt MMIO hook. SYSIC receives real boot-mode writes.
System reset retains DRAM. The supplied ROM's provenance is a separate concern:
this option does not certify that an asset is an unmodified silicon dump.

The actual-board qtest `ipod-n45-rom-test` uses synthetic ROM bytes to verify
reset-vector mapping, ROM write protection, unmodified jump-table data, absence
of staged iBoot/stubs, CPU PC zero, and retained DRAM across reset. The actual-board
qtest passed against the built QEMU (`ltm-next-n45-rom-qtest.log`). Those checks
establish board wiring; they do not establish stock ROM boot or hibernate wake.

Current default N45 boot still enters staged iBoot and uses ROM-service stand-ins.
Current PMU behavior does not implement the AP power-domain transition needed
for 1.x hibernate. Do not infer power-off or successful wake from a dark display.

## Stock firmware evidence

Read-only analysis of iPod1,1 1.1 (3A101a) found a genuine LLB in the IPSW,
`Firmware/all_flash/all_flash.n45ap.production/LLB.n45ap.RELEASE.img2`.
Its decrypted IMG2 payload is 49,152 bytes and linked at 0x22000000. The current
NOR preparer omits LLB and iBoot; their placement and the ROM's physical flash
reads still need a stock cold-boot trace before changing the default preparer.

LLB checks retained DRAM words 0x08000080/0x08000084 for 0x4d4f5358/0x53555350,
clears them, and compares the 16 bytes at 0x08000090 with its sleep token.
The successful path invokes a teardown helper with argument 4 and branches to
address zero. The helper argument has not been established as a hardware boot
mode. These are
firmware observations for debugger diagnostics, not constants or inspections
to introduce into a hardware model. They establish why a reset-vector ROM path
is necessary; they do not establish the full subsequent resume contract.

Previous native 1.x hibernate emitted `pmu go hib`, wrote PMU register 0x76=0x80
and 0x0c=0x02, then spun in `_ml_arm_sleep` with IRQ/FIQ masked. Register 0x0c
bit 1 is not currently implemented as an AP power-domain transition. An EXTON
IRQ alone cannot resume that spin. A future power-domain model must preserve
DRAM and let the real ROM/LLB execute; it must not jump to a guessed kernel
resume address.

Next native observations: cold ROM's first MMIO divergence, then bounded
pre/post-hibernate DRAM metadata at 0x08000080..0x080000c0, PMU retained state,
and the ROM/LLB path. Keep the experimental mode opt-in until stock cold boot
and a complete sleep/wake cycle are demonstrated.

## Initial native observation

A disposable cold supplied-ROM run entered PC zero, followed the ROM branch to
0xc4, and reached its first MMIO store at PC 0xcc: 0xa5 to watchdog address
0x3e300000. This demonstrates native reset-vector execution, not a completed
boot. The watchdog window is currently RAM-backed; that first write alone is
not evidence of failure. The bounded next trace should execute normal writes
and record a sequence of accesses or a repeated-PC polling loop to identify
the first unmet hardware contract. The input ROM and baseline NOR remained
unchanged, and the owned process was reaped.

The next native trace reached a real divergence: ROM loaded PLLLOCK at
0x3c500040 and compared the complete value with 1. It had enabled only PLL0
with PLLMODE 0x111 and configured PLL0CON 0x08005000, but the old model returned
0xf indefinitely. This was a false lock report for three disabled PLLs, not
an inverted status polarity. N45 now derives four lock bits from independent
enables and nonzero PDIV/MDIV. OpeniBoot's S5L8900 header confirms MDIV spans ten
bits and PDIV six; the S5L8720 eight-bit multiplier helper remains separate.
The inspected primary reference is [OpeniBoot hardware/clock.h](https://github.com/iDroid-Project/openiBoot/blob/866562fdb1cfd019bcd77885c80fbf0af65d5c15/plat-s5l8900/includes/hardware/clock.h),
with its `CLOCK1_PLLMODE_ONOFF`, `CLOCK1_MDIV`, and `CLOCK1_PDIV` definitions.
An explicit `s5l8900-pll` model property selects this contract only on CLOCK1;
the secondary CLOCK0 block retains its existing behavior.
Lock latency is still immediate. The refined board qtest passed both cases.
A corrected native cold-ROM trace observed PLLLOCK=1 and progressed beyond
the old loop to subsequent clock initialization (PC 0x274). Default direct-iBoot
regressions and the next cold-boot divergence remain to be recorded before
calling the complete ROM boot path verified. `test_n45_pll_locks.py` exercises the actual helpers/read handler under
ASan/UBSan; the board qtest also exercises the captured sequence and invalid
settings, with no guest instruction patches.

## PMU variant boundary

The N45 BCD PMU is identified as PCF50635 in the existing model; its shared
`pcf50633` type name does not establish identical register semantics. The
[NXP PCF50633 manual, table 8](https://www.freecalypso.org/pub/GSM/GTA02/PCF50633UM_6.pdf)
marks OOCSHDWN bit 1 reserved. It therefore does not establish the N45
0x0c=2 transition or the meaning of 0x76=0x80. Those require the 50635/Apple
variant contract and actual sleep-path observations; no PMU power transition
has been added on the strength of that unrelated bit description.

## Bounded follow-up trace

The corrected-lock cold ROM reached 25,836 stepped instructions, including SPI
and GPIO accesses, before an instruction read failed after the indirect branch
at 0x200024b4. The old diagnostic lost the destination by parsing the GDB error
as hexadecimal bytes. The attach-only tracer now preserves exact destination
PC, CPSR, registers and raw GDB reply when instruction memory is unavailable;
it also checks reply length and receives fragmented checksum bytes correctly.
Synthetic protocol tests pass. This is a diagnostic correction; the next native
receipt must establish the actual execution boundary before any hardware change.

## Measured SRAM aperture

The corrected diagnostic stopped cleanly at step 25,837, PC 0x22002b98,
CPSR 0x80000033, with GDB reply E14. This follows the ROM's actual indirect
branch at 0x200024b4. The board had allocated only a 4 KiB stub window at
0x22000000 and a separate 64 KiB bank at 0x22020000; the execution target
was not mapped.

The project's stock 3A101a `DeviceTree.n45ap.img2` independently specifies
AMC child window 0x1a000000 of size 0x2c000. Its arm-io range maps child
0x10000000 to parent 0x18000000, establishing physical 0x22000000 and
176 KiB. The recursive decoder consumes the full 32,040-byte DT; decoded
SHA256 is `bcf1afb87c368178fd6401cf41b5ade4686649a90edec33fc571bc53c3021896`.
The metadata-only proof is `/Users/shg/Developer/ltm-fidelity/evidence/ltm-evm-startup-next/n45-sram-dt-proof.json`.
This agrees with the independently inspected S5LBox DT observations; the
512 KiB assertion in another emulator's documentation was not adopted.

The opt-in ROM mode now exposes that contiguous bank. It no longer allocates
stub-sized SRAM or the extra unproven 16 KiB above the physical aperture.
The default direct-boot mapping is unchanged. The rebuilt board suite passes
all three cases, including first/gap/bank-join/final-word writes, independent
addresses, ignored writes outside the aperture and retention across reset:
`/Users/shg/Developer/ltm-fidelity/evidence/ltm-next-n45-sram-qtest.log`. Immutable hardware artifact SHA256:
`37a388f6ce73d83b0639e0570019b183afdc8ff3fc9358beec4d0ee1c126ad50`.
The native `/Users/shg/Developer/ltm-fidelity/evidence/ltm-n45-rom-contiguous-sram/rom-trace.json` passes
the old instruction-memory boundary and reaches the 100,000-step bound. It
records 418 MMIO accesses and two cold-start sequences; the final PC is the
normal ROM RAM-clearing loop at 0x2000039c. This does not prove successful
LLB loading or explain the restart. An accelerated read-only breakpoint probe
will record the actual indirect target contents and subsequent control flow
before selecting another hardware correction.

The same N45 DT identifies `/arm-io/nor-flash` as `nor-flash,cfi`, with
child 0x1c000000/size 0x100000 translated to physical 0x24000000. SPI0
at physical 0x3c300000 instead has an LCD transport child. This differs
from S5LBox's iPhone1,2 DT SPI-NOR topology; its NOR wiring must not be
copied to N45. The ROM trace's SPI attempt/underflow is a research lead
about boot selection or fallback, not evidence to replace the CFI mapping.

## Missing executable component

The accelerated native `/Users/shg/Developer/ltm-fidelity/evidence/ltm-n45-rom-entry-boundary/rom-trace.json`
completes in 0.132 seconds: the supplied ROM literal is 0x22002b99, the actual
Thumb entry is 0x22002b98, its first 32 bytes are all zero, and 24 stepped
instructions execute `movs r0,r0` before ROM address zero is reentered. The
SRAM aperture correction therefore exposes a missing instruction component;
it does not deliver a working ROM boot or retained-RAM wake.

Bounded inspection of this supplied ROM's startup shows its data relocation
from 0x2000bd00 to 0x22020000, followed by zeroing of 0x22020804..0x22026c84.
That sequence does not populate the lower SRAM library targets. Multiple ROM
veneers reference Thumb functions there. Their provenance and initialization
remain unresolved; supplying arbitrary LLB bytes or injecting routines would
not establish the physical contract.

The original emulator author independently documented the same limitation in
[the 2022 N45 boot-chain account](https://devos50.github.io/blog/2022/ipod-touch-qemu/):
the ROM and LLB jump into unavailable code at 0x22000000. His explanation that
this might be fused code is speculation, not a confirmed hardware mechanism.
The public rax reuse lead does not resolve it: pinned commit
`771a6c1514e00b3054d088426841a32eec2884e6`
[boot.rs](https://github.com/HexRaysSA/rax/blob/771a6c1514e00b3054d088426841a32eec2884e6/src/machine/s5l8900/boot.rs)
stages iBoot and installs verify/decrypt guest stubs at the same SRAM addresses.
No such substitution has been adopted in the opt-in ROM mode. A complete,
independently sourced executable component or physical reset observations are
needed before advancing this boot/wake path.


## Default route control

The pinned contiguous-SRAM candidate's default N45 route retains its legacy
RAM mapping and direct iBoot behavior. Durable `n45-default-control` verifies
boot/display pixels remain lit and stable for15seconds, then the existing host
power gesture causes actual guest PMU shutdown and exit0 at43.5seconds. This
qualifies default display boot and shutdown only. Filesystem checking is
explicitly SKIP because this physical format is unsupported by the checker;
no Home screen, full filesystem, authentic ROM boot or retained-RAM wake claim
is made. The ROM-mode SRAM mapping remains opt-in.
