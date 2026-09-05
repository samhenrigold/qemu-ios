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

Still to verify before implementation is accepted:

- Confirm the ADC completion event registration in this kernel (the older
  firmware analysis identifies EVENT_B bit 5).
- Wire masked PMU events through GPIO IRQ 0x61, with coherent read-to-clear and
  group acknowledgment. The current button path manually latches SYSIC state.
- Sweep calibrated voltage counts and compare guest battery properties; derive
  charging status from observed driver decoding.
- Verify channel 6's USB charging-identification sequence and input selection.

## Existing behavior to preserve

RTC counter reads at `0x5c..0x5f` and offset writes at `0x64..0x67` already work.
The guest reaches its final native shutdown command `0x6f=0x90`; shutdown no
longer needs an ADC-related workaround. Runtime power-source status uses
`0x04` bit 3 for USB cable presence. Other status bits remain unverified.
