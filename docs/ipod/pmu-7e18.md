# D1759 PMU: 7E18 evidence for the next hardware track

The model's historical PCF50633 name does not identify the register map used by
AppleD1759PMU. These observations come from the running 3.1.3 kernel, with virtual
addresses resolved against a RAM dump; they refine the supplied project plan.

## ADC request and result

`c05fb62c` serializes conversions through a workloop command gate. The low nibble
of register `0x40` selects a channel. The driver preserves its configured upper
bits when issuing a request.

For channel 3, `c05fb688` first writes bit 5 (`0x23` in the current guest trace).
It arms an 80 ms timer and waits for state **3**, the timeout callback's value
(`c05fa294`). This is a settling phase, not an ADC result interrupt. Raising a
conversion-complete interrupt here would set state **2** (`c05fa2d4`) while this
phase waits for **3**. The implementation must preserve this distinction rather
than signal completion for both command bits as the original plan suggested.

The actual conversion sets bit 4 (`c05fb6fc`, observed `0x13`). The driver then
arms a 30 ms timeout and waits for its state to change. A successful completion
reads two bytes starting at `0x41` (`c05fb77c`) and reconstructs the ten-bit result:

```
counts = (register_0x41 & 3) | (register_0x42 << 2)
```

The ADCFunction wrapper at `c05fa42c` shifts that result right by two before
returning it to a platform-function consumer. The direct power-source consumer
at `c05ff73c` calls the conversion interface and converts counts to a resistance
using `counts * 20 * scale / 1024`. Battery voltage, thermistor and accessory
channels therefore need separate values; one arbitrary battery percentage
cannot safely stand in for every ADC channel.

The event dispatcher at `c05fc17c` reads three bytes from `0x01`; it tests bit 5
of the second byte at `c05fc1d0` and invokes the completion callback at
`c05fc1e4`. EVENT_B bit 5 is therefore confirmed on 7E18 too. The driver writes
mask B (`0x08`) to `0xdf` at boot, enabling precisely that bit.

Battery voltage is **channel 4**, not channel 3: `c05ff878` selects it and
`c05ff884..c05ff8a0` computes `2500 + counts * 2000 / 1024` millivolts. The
thermistor read at `c05ff81c` uses channel 2 with scale 2500. Calibration must
still establish the guest's voltage-to-percentage curve and filtering delay.

The implemented converter samples on bit-4 START and completes after 1 ms of
virtual time. PMU output is the OR of unmasked EVENT_A/B/C bits; event reads
clear those bits and recompute the line. It connects to SYSIC pin 97. GPIO masks
must allow the guest to mask then acknowledge a held PMU line while its I2C
workloop runs; unmasking re-latches a still-held request. Relatching masked
levels on every ACK trapped the guest in its GPIO dispatcher during boot.

ASan/UBSan checks cover conversions, masks, read-to-clear, cable transitions,
reset, and shared GPIO handling. Live 7E18 validation in
`/tmp/it-blitz-spore-54671` reached home, locked at 100/300/800 ms press lengths,
woke through Home, unlocked, and completed native shutdown with QEMU exit 0.
The runtime `battery-adc` control accepts raw ten-bit channel-4 counts, with 850
as the default. It deliberately does not claim a calibrated percentage yet.

Remaining calibration:

- Sweep calibrated voltage counts and compare guest battery properties; derive
  charging status from observed driver decoding.
- Verify channel 6's USB charging-identification sequence and input selection.

## Existing behavior to preserve

RTC counter reads at `0x5c..0x5f` and offset writes at `0x64..0x67` already work.
The guest reaches its final native shutdown command `0x6f=0x90`; shutdown no
longer needs an ADC-related workaround. Runtime power-source status uses
`0x04` bit 3 for USB cable presence. Other status bits remain unverified.

## Charging status and filtering

`c05ff45c` reads the power status block and tests **register 0x05 bits 1/2**
at `c05ff4a0..4b8` to update IsCharging. Leaving both bits zero made the guest
stop charging and report fully charged after its first status poll, regardless
of voltage. The model now reports bit 1 while USB power is present and the
charge-disable bits in 0x0a are clear. The guest charge-control routine at
`c05ff5f0` clears 0x0a[3:2] before selecting its permitted charging current and
sets bit 3 when USB charging is unavailable.

The real IOPMPowerSource dictionary provides the `0003-default` battery table.
At ADC 660, a fresh guest reports 3789 mV, BootCapacityEstimate 20 and
CurrentCapacity 20 (`/tmp/it-battery-cold20.log`); native shutdown passes.
Connected operation has a three-minute measurement interval and a four-sample
capacity filter (`c0600430`, `c05ffa48`). In a live 850-to-750 sweep, voltage
changed from 4160 to 3964 mV after about three minutes and capacity changed from
95 to 82 while IsCharging stayed true. Immediate lockdown battery queries can
lag the IOPMPowerSource dictionary. Percentage acceptance must account for
these native measurement/filtering intervals.

## Recovery after probing an absent I2C slave

With `IT_I2C_NAK=1`, unplugged operation exposed a controller bug: after an
absent-address 0x29 probe set LASTBIT, `I2CDS=0xe6` was discarded. START therefore
kept using stale data 0x52/address 0x29, and PMU accesses failed indefinitely
(`e00002e9`). The data register now always stages writes even when the previous
transfer NAKed; bus data delivery still respects NAK. A sanitizer regression
replays the absent-device/PMU sequence. `IT_I2C_TRACE` records register accesses,
latched address and active state for diagnosing future controller issues.

`/tmp/it-blitz-spore-56813` passed boot with real absent-device NAKs, USB detach
and reattach, power-button lock, Home wake and touch unlock, then guest-confirmed
shutdown. Its auto-lock test did not engage because the inherited demo preference
`SBAutoDimTime=-1` remained set; this is distinct from the now-fixed PMU bus wedge.

## Host controls

`battery-level=0..100` converts a voltage target through the guest's published
0003-default table. `battery-adc=0..1023` remains available for raw diagnostics.
The reported host target is not the guest's filtered CurrentCapacity. Plateaus
in the real table and ten-bit quantization mean arbitrary percentages are
approximate; calibrated targets 20 and 60 report exactly on fresh boots.
`battery-charging=auto|on|off` controls the charger phase. All modes require
external USB power and respect guest charge-disable bits. Auto stops charging
at the full-voltage target; on can keep the charging phase active there.
Changing the mode latches a charger event (EVENT_C bit 2).

`/tmp/it-blitz-spore-57272` booted at a 60-percent target, reported 60%, accepted
off/on/auto with matching guest IsCharging values, accepted a 100-percent
voltage target and stopped auto charging, then completed native shutdown.
The 100-percent fresh-voltage check reported BootCapacityEstimate 100 at 4199 mV;
while charging the guest deliberately caps CurrentCapacity at 95 until charge
completion. Runtime voltage changes retain the native measurement/filter delay.

## Suspend-to-RAM handoff (investigation)

Removing the demo preferences for both auto-dim and auto-lock allows actual
system sleep after USB detach. In `/tmp/it-blitz-spore-60361`, 7E18 disables the
LCD, USB, GPU and digitizer, logs `System Sleep` and `pmu go hib`, writes PMU
`0x6f=0x80`, then changes `0x0a` from `0x10` to `0x0a`. This differs from native
shutdown's `0x6f=0x90`; it must not terminate the emulator or discard RAM.

The CPU ends at `c0061eb0`, an unconditional branch with IRQ/FIQ masked, after
saving time and flushing caches. An ordinary PMU interrupt cannot wake it.
The retained RAM vector at physical `0x08000000` is an ARM trampoline; its entry
word at offset `0x24` changes from `0x08069000` (cold startup) to `0x08069008`
(resume). Offset `0x28` retains the physical boot/context argument `0x087ad000`.
The two entries select different continuations, rebuild CPU translation/control
state and enter the kernel. A full machine reset would destroy this state by
staging the normal boot chain again. CPU-only reentry through the retained
vector is under investigation; no working native hibernate wake is claimed yet.

A CPU-only reset experiment in `/tmp/it-blitz-spore-61103` entered that vector
and reached the native `pmu wake events: hold` handler. It subsequently reported
an undefined kernel instruction at `c001be04`, with a saved CPSR of `0x113`,
before the display returned. This narrows the missing work to resume CPU/device
state rather than button interrupt delivery. The experimental reentry code was
removed; it is not part of normal operation or the sound/menu app package.

## Dock accessory ID and silent speaker regression

The 7E18 device tree's `/dock/function-read_acc` is `<PMU phandle, 'pmuA', 3>`:
accessory identification uses ADC channel 3. Leaving that input at zero caused
AudioSessionGetProperty('rout') to report `LineOut` on an ordinary USB-connected
emulator. Raw I2S contained PCM (peak 3277), but the LM48821 remained at control
00 and the entire host WAV was silent. Volume-up presses did not change this.

Channel 3 now defaults to 1023 for an open accessory-ID input. With that single
change the same candidate NAND reports `Speaker`; the default Harness stereo
check captures 5.99 seconds of non-silent audio with 440.0 Hz left and 880.0 Hz
right. Boot and the agent binary-transfer check pass in the same run:
`/tmp/it-regress-open-dock-native.log`. The ADC regression checks the separate
channel value, settling/conversion encoding and persistence across reset. This
fix does not implement headset/microphone support or dock accessories.

## Automatic drain

`battery-drain=0..100` sets percent per emulated minute; zero (default) disables
it. ADC/status reads and host target queries sample elapsed virtual time. No
periodic timer or synthetic measurement interrupt is added. Pausing freezes
drain; USB power freezes it unless `battery-charging=off`. This models discharge
only, with the guest retaining its own voltage compensation and capacity filter.
Fractional levels survive individual ADC polls and v4 migration. Older saved
states restore with drain disabled. The macOS battery-config bridge queues all
three controls on the emulator thread and retains the older two-argument ABI.

`test_pmu_adc.py` exercises fractional drain, pause, cable/charging changes,
bounds, old/current restore and malformed migration. `test_battery_bridge.py`
checks input bounds and queued delivery. `test_battery_guest.py` uses an isolated
native guest and reads IOPMPowerSource through the read-only `itbattery` helper.

Native discharge acceptance passes at `/tmp/it-battery-drain-native-v5.log`.
7E18 may defer ADC measurements indefinitely when externally powered but
charging is forced off. Normal unplugged discharge samples voltage after about
a minute. USB unplugging also changes its voltage compensation; tests account
for that instead of equating the host target with the filtered guest estimate.
