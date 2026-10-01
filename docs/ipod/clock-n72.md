# N72 root clock and watchdog investigation

The root clock controller at 0x3c500000 now derives its peripheral output through
QEMU's Clock API. CONFIG0 selects PLL0..2; PLLMODE enables each PLL and selects
the 24 MHz epoch-1 or 27 MHz reference. MDIV/PDIV and CONFIG1's peripheral
divider determine PCLK. The selected main PLL is sampled before SDIV.
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
frequency for this configuration is 266 MHz. Baseline and candidate both reach
the same LLB polling loop at the 60-second capture. This trace establishes clock
programming, **not** a successful 5F138 kernel boot.

The ordinary 7E18 direct-iBoot path instead leaves root PLL/divider registers
zero throughout two native boots. Its root writes adjust peripheral gates and
idle control, but do not initialize the PLLs. The model therefore reports a
stopped PCLK; it does not substitute a guessed running frequency. This exposes
a missing earlier-stage handoff in that boot shortcut.

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

Timed watchdog expiry has **not** been enabled. The exact immediate-reset command
retains its prior behavior. Making a new expiry timer run against an invented
clock or a borrowed counter width would obscure these gaps.

## Verification

Production-board `tests/qtest/ipod-clock-test.c` covers independent PLL lock
states, invalid/disabled PLLs, read-only lock status, lock-count readback, reset,
both references, peripheral dividers, PLL selection and actual VMState save/load
with output recomputation: 3/3 pass. Registered model tier: 6/6 pass, no skips.
The native 7E18 import/retry/cold-reopen test passes on this candidate, including
full tags, one song after duplicate import and MediaPlayer-decoded artwork.

Retained local evidence:

- `/private/tmp/ltm-n72-clock-rom-trace2`: baseline SecureROM/LLB register trace.
- `/private/tmp/ltm-n72-clock-rom-candidate`: candidate trace and register captures.
- `/private/tmp/ltm-n72-wdt-clock-trace`: native direct-iBoot clock/feed trace.
- `/private/tmp/ltm-n72-clock-qtest.log`: lock, frequency and migration contracts.
- `/private/tmp/ltm-overnight-models-clock.log`: registered model tier.
- `/private/tmp/ltm-n72-clock-native`: fresh/reopened native media proof.
