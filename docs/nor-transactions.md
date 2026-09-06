# NOR transaction boundaries

The current NOR model frames reads using a non-0xff sentinel and ignores program
and erase. That cannot safely support variable-length page programming: the
next command must be distinguished using chip select, not a data byte.

A native 7E18 `nvram ltm-nor-test=trace` on a disposable `nand-agent-v4` overlay
produced this repeated sequence:

```
ipod_touch_spi_cs controller 0 pin 0x00000000
ipod_touch_gpio_write offset 0x1e0 value 0x0000000e
ipod_touch_nor_command command 0x06
ipod_touch_gpio_write offset 0x1e0 value 0x0000000f
ipod_touch_spi_cs controller 0 pin 0x00000000
ipod_touch_gpio_write offset 0x1e0 value 0x0000000e
ipod_touch_nor_command command 0x02
ipod_touch_gpio_write offset 0x1e0 value 0x0000000f
```

The same boundaries surround status reads, status writes and 0x20 erase. GPIO
FSEL encodes pad in bits 16+, pin in bits 8+, and output-low/high as 0xe/0xf.
The NOR transaction selects pad 0 pin 0. GPIO writes are currently discarded;
SPI R_PIN is not the missing transaction signal. The local Samsung GPIO/flash
reference drivers independently describe this FSEL encoding and separate chip
select from the byte transfer.

Evidence: `/tmp/it-nor-trace-native.log`, trace in
`/var/folders/tp/360v5_ln3lxg5x66gf0rqc540000gn/T/it-nor-trace-s950g1kt/device/qemu.log`.
The guest command reads back its new variable from the kernel's NVRAM cache;
that is **not** proof of flash persistence. Guest-confirmed shutdown passed.

Enable the three disabled-by-default QEMU events with HMP:

```
log trace:ipod_touch_nor_command,trace:ipod_touch_spi_cs,trace:ipod_touch_gpio_write
```

The advertised flash is AT25DF081A (JEDEC 1f4502). Its
[manufacturer datasheet](https://www.renesas.com/en/document/dst/at25df081a-datasheet)
requires chip-select deassertion to finish page programming. The program buffer
is 256 bytes; status includes write-enable and erase/program-error bits. The
next implementation must use real GPIO transaction boundaries and preserve
the original NOR image rather than writing into the shared base file.

## GPIO wiring checkpoint

FSEL output-low/high now updates bounded pad/pin state and drives QEMU GPIO
outputs. Pad 0 pin 0 connects to NOR's active-low SSI chip select. NOR clears
transaction state at the select boundary; read data no longer treats MOSI bytes
as command sentinels. Reset deasserts NOR select and clears the write latch.
The digitizer retains its existing framing; SPI R_PIN is not routed to NOR.

`tests/ipod/test_nor_transactions.py` passes ASan/UBSan for FSEL bounds and output
levels, command-looking bytes during reads, JEDEC ID and interrupted addresses.
NVRAM partition tests pass. Native 7E18 NVRAM transactions and confirmed shutdown
pass (`/tmp/it-nor-cs-native.log`); a fresh 5F138 ROM boot reaches Home
(`/tmp/it-nor-cs-legacy.log`, inspected frame-3.png under the reported output).
Program/erase are still no-ops at this checkpoint; persistence is not claimed.
