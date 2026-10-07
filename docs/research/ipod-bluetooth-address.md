# N72 BCM4325 HCI address programming

The native 7E18 BlueTool sequence downloads the minidriver, sends vendor
command `0xfc01` with six bytes, and then sends `HCI_Reset`. The old UART1 HCI
model acknowledged that write without storing it; `Read_BD_ADDR` (`0x1009`)
always returned the same controller-wide placeholder.

Linux's [Broadcom HCI driver](https://github.com/torvalds/linux/blob/master/drivers/bluetooth/btbcm.c)
uses `0xfc01` and six bytes in `btbcm_set_bdaddr`. The model now stores that
address in HCI byte order and returns it on reads. Malformed write lengths and
nonempty read parameters report Invalid HCI Command Parameters (`0x12`) without
changing the address. Retention across HCI_Reset follows the observed BlueTool
provisioning sequence; board reset restores the existing unprovisioned default
and clears parser/reply state. VMState version 2 carries the programmed address;
version 1 restores its previous default rather than inventing a provisioned MAC.

`tests/qtest/ipod-bt-test.c` drives the production board's UART1 registers:
fragmented H4 writes, delayed replies, readback, malformed parameters, HCI reset,
board reset while a command/reply is pending, and migration with a partially
received read command. All three tests pass. The registered model gate passes
all eight suites without skips (`/private/tmp/ltm-overnight-models-bt2/`).

Native 7E18 proof is at `/private/tmp/ltm-n72-bt-native/`. BlueTool programs
`02:d9:ea:c1:81:53`, matching that fixture's generated identity. All four
lockdown identity fields match the fixture, early BSD mount logging is present,
and three BluetoothManager probes attach in 9.7–10.5 ms with enabled/powered
queries returning in at most 0.4 ms.

This is still an HCI-level stand-in: no BCM CPU/patchram execution, radio,
ACL/SCO transport or peer connections. The iBoot UART3→UART1 string rewrite
remains a separate compatibility patch. Address readback does not populate
DeviceTree properties before the guest HCI stack starts, so this change does not
justify removing that patch or claiming a faithful Bluetooth core.
