# N45 PCF50635 interrupt contract

The N45 uses a PCF50635. The shared model's historical `pcf50633` name also
covers the N72 D1759, but their interrupt maps differ. The BCD-calendar variant
uses INT1..5 at 0x02..0x06 and INT1M..5M at 0x07..0x0b; the D1759 continues
using three EVENT banks at 0x01..0x03. Status is latched while masked; reading
status clears that bank; the output is the OR of unmasked status.

Stock 3A101a initializes all five masks. Its later INT2M writes include 0xbf,
0x3f and 0x3b, enabling EXTON1 rising. Hold/Power drives the board's EXTON1
input: rising is INT2 bit 2 and falling bit 3. USB insertion/removal latch INT1
bits 2/3. The live USB level remains MBCS1 at 0x4b, independently of event reads.
The register/bit assignments agree with the
[PCF50633 family register definitions](https://github.com/torvalds/linux/blob/v6.1/include/linux/mfd/pcf50633/core.h)
and the stock driver's five-mask trace. These facts inform the model; Linux
implementation code is not copied.

`tests/qtest/ipod-touch-irq-test.c` exercises the actual N45 board, sending PMU
transactions through the production I2C controller at 0x3c900000 and power
button input through QMP. The masked event survives until INT2M is enabled;
GPIO source 0x55 (group 2 bit 21) then asserts VIC source 0x1f. Parent ACK cannot
consume the PMU's level; reading INT2 clears it; the falling edge relatches it.
The qtest also checks all five power-on masks and all five clear event banks.
`tests/slice/ipod-pmu-adc.c` covers the five independent banks and D1759 regression.

Reset clears the configured shutdown register (N45 0x0c), rather than treating
0x0a, its INT4M, as the D1759 shutdown register. VMState version 5 retains the
EXTON1 input level. Older N45 snapshots are refused because their interrupt
register meanings differ; older D1759 snapshots retain their previous support.

Native 3A101a with a corrected four-page generated FTL base completes startup
without an abort. This does **not** establish hibernate resume: the retained
170-second power/Home run has a black display and ends in a terminal branch
with IRQ/FIQ masked after `pmu go hib` and 0x0c=2. The retained-RAM boot/resume
handoff needs separate hardware evidence. Native 7E18 completes music import,
repeat reconciliation and cold reopen on this PMU binary. Evidence paths are
listed in the app's consolidated remaining-work report.

The PCF ADC, charger/regulator sequencing and retained-RAM resume are still
incomplete. This change implements an interrupt contract, not the whole PMU.
