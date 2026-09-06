# NOR transaction boundaries

The original NOR model framed reads using a non-0xff sentinel and ignored program
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
The NOR transaction selects pad 0 pin 0. GPIO writes were discarded;
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

## Program/erase checkpoint

NOR now implements write-enable/disable, status and global protection, page
programming with page-buffer wrap and flash AND semantics, and 4/32/64 KiB erase.
Mutations commit when GPIO releases chip select. The fixed 1 MiB flash array and
in-flight command state are included in VMState v2, with bounds checked on load.
Version 1 snapshots reload the original image because they contained no writes.
`test_nor_snapshot.py` verifies native migration of flash contents and an
unfinished page program using paused machines before any GL context exists.

Operations complete synchronously; busy timing and per-sector protection are
not modeled. Writes currently survive machine reset in RAM only. The shared
base image is never modified; cross-process persistence needs the planned
optional overlay.

The sanitizer check covers protection, write-enable, interrupted commands,
page wrap, erase bounds, read wrap, malformed snapshot fields and missing media.
A native 7E18 reboot reads back the new NVRAM variable after the guest emits
status-unprotect, erase and page-program commands
(`/tmp/it-nor-program-reboot-direct.log`). Merely setting a variable and issuing
a host reset loses the kernel's unflushed cache; that is not a flash test.
`test_nor_guest.py` exercises the native flush/reboot path on a disposable NAND.

The checked-in native guest test passes through confirmed untethered shutdown
with QEMU exit 0 in 55.2 seconds (`/tmp/it-nor-guest-final.log`).

## Private persistent NOR

`-M iPod-Touch,...,nor-rw=/path/to/private-nor.bin` selects an existing 1 MiB
private copy. Without it, writes remain in memory. QEMU opens the copy as raw
storage with exclusive write permissions; base-image aliases (including hard
links), missing/wrong-sized files and concurrent writers are rejected. For CLI
use, copy the selected NOR once before supplying `nor-rw`. Never use the base
image itself. Light Touch creates the copy automatically as `nor.bin` inside
its device overlay, preserving it across launches and removing it on factory
erase alongside the NAND overlay.

Program/erase writes use the block backend's durable write flag. Host I/O
failure sets EPE, stops the VM and remains visible through Light Touch's storage
failure status across reset. Snapshot restore synchronizes the saved flash on
the first bus transaction, after incoming block backends activate.

`test_nor_snapshot.py --persistent` covers migration, an in-flight page program,
a fresh process, base preservation and rejected files/concurrent writers.
`test_nor_guest.py --persistent` verifies native iOS NVRAM across reboot and a
complete process restart; both guests shut down cleanly (82.5 seconds total,
`/tmp/it-nor-persistent-guest.log`). Synchronous durable writes are appropriate
for the small, infrequent NVRAM workload; NOR timing remains instantaneous.
