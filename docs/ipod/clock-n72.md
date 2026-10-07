# N72 root clock and watchdog investigation

The root clock controller at 0x3c500000 derives its peripheral output through
QEMU's Clock API. CONFIG0 selects oscillator bypass or PLL0..2. The root model
has a typed link to the board's actual ChipID fuse device: CHIPID_INFO bit 0
selects 12 MHz (clear) or 24 MHz (set), while PLLMODE can select the independent
27 MHz reference per PLL. MDIV/PDIV, SDIV and CONFIG1's peripheral divider
determine PCLK. PLL0 uses SDIV; PLL1/2 use SDIV+1. This corrects the earlier
unsupported assumption that the selected PLL ignores SDIV.
Disabled or unconfigured PLLs do not report locked, and their output is stopped.
Lock-count registers retain their programmed values. PLL settling still occurs
immediately; analog timing is not modeled.

Register encodings are corroborated by OpeniBoot's S5L8720
[clock header](https://github.com/iDroid-Project/openiBoot/blob/master/plat-s5l8720/includes/hardware/clock.h)
and [clock reader](https://github.com/iDroid-Project/openiBoot/blob/master/plat-s5l8720/clock.c).
These are hardware references, not copied implementation. The S5L8900 and
secondary block keep their existing behavior; this change does not generalize
the N72 encoding to them.

## Boot-stage evidence

The isolated SecureROM/5F138 NOR trace writes PLL0=0x03008501, PLLMODE=0x10001,
CONFIG1=0x424242 and CONFIG0=0x1000. The later LLB programs PLL0=0x03008500,
PLL1=0x06005101, PLLMODE=0x30003 and CONFIG0=0x100a. The derived peripheral
frequency for this late SDIV=0 configuration is 133 MHz with the default
epoch-0 fuse, or 266 MHz with the explicit epoch-1 fuse. The earlier test
assumed epoch 1 despite the actual board supplying epoch 0; that assertion
has been corrected. Baseline and candidate both reach
the same LLB polling loop at the 60-second capture. This trace establishes clock
programming, **not** a successful 5F138 kernel boot.

The ordinary 7E18 direct-iBoot path instead leaves root PLL/divider registers
zero throughout two native boots. Its root writes adjust peripheral gates and
idle control, but do not initialize the PLLs. The model originally reported a stopped PCLK here. Stock iBoot
proves source zero deliberately selects oscillator bypass; the candidate now
reports the physical fuse-selected oscillator with its programmed peripheral
divider. This does not invent a PLL setup for the direct-boot shortcut.

The Clock output is recomputed from restored registers after VMState load; it
is not an independent saved frequency. Existing peripherals still use their
existing fixed timebases. Connecting them to PCLK requires their own source,
divider, gate and boot-stage contracts; this change does not fix all timer rates.

## Watchdog boundary

Register-write tracing now includes QEMU virtual time. Native 7E18 regularly
writes 0x001f4a00 at about 2.797 virtual seconds apart. OpeniBoot's
[watchdog driver](https://github.com/iDroid-Project/openiBoot/blob/master/plat-s5l8720/wdt.c)
uses the same control value, but its enable function returns before doing any
work. Its intended prescaler/divider/period calculation is a useful lead, not
proof of live N72 watchdog timing. An 11-bit counter claim for older S5L870x
must not be assumed to apply to S5L8720. Counter width, clock selector behavior,
overflow/reset timing and interrupt mode still need N72 evidence.

The later stock7E18 provider audit narrows the source: watchdog provider index0
uses DT clock ID2, the cached AppleS5L8720XIO table entry populated from cpu0
`bus-frequency`. It is not a direct read of the modeled root PCLK. This mapping
is established for7E18 only; actual bootloader publication/gating, CNT movement,
kick/reload/disable semantics and overflow still require independent observation.
Evidence: `~/Developer/ltm-evidence/watchdog-provider-2026-10-01`.

Timed watchdog expiry has **not** been enabled. The exact immediate-reset command
retains its prior behavior. Making a new expiry timer run against an invented
clock or a borrowed counter width would obscure these gaps.

## Verification

Production-board `tests/qtest/ipod-clock-test.c` covers independent PLL lock
states, invalid/disabled PLLs, read-only lock status, lock-count readback, reset,
both references, peripheral dividers, PLL selection and actual VMState save/load
with output recomputation: the original 3/3 passed before the reference
correction. The expanded four-case board suite has now passed a fresh run.
The original registered model tier was 6/6, no skips; do not apply that old
receipt to the revised clock model.
The native 7E18 import/retry/cold-reopen test passes on this candidate, including
full tags, one song after duplicate import and MediaPlayer-decoded artwork.

Retained local evidence:

- `~/Developer/ltm-fidelity/evidence/ltm-n72-clock-rom-trace2`: baseline SecureROM/LLB register trace.
- `~/Developer/ltm-fidelity/evidence/ltm-n72-clock-rom-candidate`: candidate trace and register captures.
- `~/Developer/ltm-fidelity/evidence/ltm-n72-wdt-clock-trace`: native direct-iBoot clock/feed trace.
- `~/Developer/ltm-fidelity/evidence/ltm-n72-clock-qtest.log`: lock, frequency and migration contracts.
- `~/Developer/ltm-fidelity/evidence/ltm-overnight-models-clock.log`: registered model tier.
- `~/Developer/ltm-fidelity/evidence/ltm-n72-clock-native`: fresh/reopened native media proof.

## Later stock clock-publication evidence (2026-10-02)

A read-only watchdog investigation found a concrete qualification to the root
clock model above. Stock 7E18 iBoot explicitly stores its base oscillator
frequency immediately before a three-entry PLL frequency array. Its CONFIG0
selector subtracts one; selector zero deliberately reads this initialized base
slot. Therefore the model's stopped output for selector zero describes the
current implementation, not the firmware's interpretation of oscillator bypass.

The stock base-frequency getter reads CHIPID_INFO at 0x3d100008 bit 0 and selects
12 MHz when clear, 24 MHz when set. The primary
[OpeniBoot CHIPID header](https://github.com/iDroid-Project/openiBoot/blob/866562fdb1cfd019bcd77885c80fbf0af65d5c15/plat-s5l8720/includes/hardware/chipid.h)
identifies this as the security-epoch fuse; its
[clock header](https://github.com/iDroid-Project/openiBoot/blob/866562fdb1cfd019bcd77885c80fbf0af65d5c15/plat-s5l8720/includes/hardware/clock.h)
corroborates the two references. The current default ChipID word2 is 0x87200004,
with that bit clear, while the root model's epoch reference currently assumes
24 MHz. Board tests that assume epoch 1 must explicitly select the matching
fuse rather than silently testing a different part from the default board.

The stock DT publisher obtains `bus-frequency` from clock getter index 3,
which reads the selected source divided by the bus divider. Thus a native
12 MHz watchdog provider can be explained by a real oscillator bypass path;
it is not proof of a fixed watchdog input or permission to introduce a guessed
expiry timer. The final provider return still needs a native capture. An
existing DT snapshot at the earlier serial-write stage contains zero clock
fields and is not a qualified final handoff observation.

Bounded diagnostic instructions and hashes are retained locally in
`~/Developer/ltm-fidelity/evidence/ltm-evm-startup-next/n72-clock-bypass-proof.json`; no new native
run or model change was performed for this investigation. Stock iBoot's SDIV
arithmetic also needs comparison with the existing main-PLL-before-SDIV
assumption before extending that behavior beyond the captured late SDIV=0
configuration. CNT width, movement, reload, selectors and expiry remain
unsupported by these software frequency-publication findings.

## Reference/SDIV candidate qualification (2026-10-02)

The reference correction is now implemented with a required typed ChipID link,
not a guessed epoch property or firmware build tag. The board constructs its
fuse device before the root clock; unrelated fuse/ECID fields are unchanged.
S5L8900 and the secondary blocks are unaffected. No peripheral consumer or
watchdog countdown has been connected to this revised output.

The bounded stock 7E18 frequency-reader instructions prove PLLMODE bit n+4
selects 27 MHz when set and the CHIPID-derived oscillator when clear. Its
three PLL cases assign a zero SDIV bias for PLL0 and one for PLL1/2 before
computing the shift divisor. This disagrees with OpeniBoot's selected-main
PLL special case. The previous model/test assertion that PLL0 SDIV=7 leaves
frequency unchanged had no native SDIV-nonzero evidence and is removed.
The bootloader software arithmetic supports the revised contract; analog
measurement, settling latency and dynamic gate propagation remain unmodeled.

`test_n72_clock_reference.py` executes the production derivation with ASan/UBSan:
both fuse references, bypass with dividers, all eight PLL0 SDIV values, auxiliary
PLL divisors, independent 27 MHz input, missing fuse link and invalid/disabled
selected PLLs pass. The actual-board qtest now covers default epoch0, explicit
epoch1, reset and restored registers. The rebuilt actual-board clock suite
passed all four cases on October 2; the unchanged N45 ROM suite also passed
both cases. Exact commands and artifact scope are recorded in the app worktree
`docs/fidelity-evidence-2026-10-02.md`. The later native qualification below
uses current clock/board objects. Old native media/clock receipts above describe
the previous candidate only.

The current-clock private AES/CoreAnimation artifact used by
`~/Developer/ltm-fidelity/evidence/ltm-next-notes-host-native` reaches the 2.1 home screen and passes
identity, activation, AFC/install, two guest-confirmed PMU shutdowns and cold
persistence. Its separate Notes keyboard gate fails, so this is qualified
2.1 lifecycle evidence rather than a complete application/UI pass. Current
7E18 strict EVM capture reaches kernel/MBX initialization, but deliberately
stalls at unsupported allocation and does not qualify home/shutdown. The
previous bfac 7E18 full gate linked different clock/board objects; its old
8/8 receipt must not be reused as native qualification of this correction.

The subsequent current-clock combined artifact
`742448356ef29bc50aca6c356bc2456de60ad5d7d5c2c271870276129de9bbac`
passes all eight native 7E18 checks in
`~/Developer/ltm-fidelity/evidence/ltm-next-current-hardware-ios313`,
including two guest-PMU-confirmed host shutdowns, cold persistence and fsck0.
This qualifies the reference correction in the existing direct-iBoot/default
graphics path. It does not qualify strict GPU execution, stock restore, or the
new authenticated engineering ROM chain.
