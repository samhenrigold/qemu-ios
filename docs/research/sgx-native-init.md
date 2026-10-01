# Stock K48 / 7B500 SGX initialization barrier

The stock restored cold-boot gate still fails its lockdown identity judge after
300 seconds. A read-only gdbstub investigation on 2026-09-30 locates an actual
GPU shared-memory handshake; it does not establish that this is the only
remaining blocker. No guest instruction, guest data or completion bit was changed.
The prepared-device path disables SGX matching and therefore does not test this
native path (see `h2fmi-raw-reads.md`).

## Measured mapping

The final paused sample had PC `0xc0695f6a`, `r2=0xed38d000`, and
`ldr r3,[r2,#0x18]; ands r5,r3,#1; bne` looping with bit 0 set.
The stock decompressed kernelcache SHA256 was
`bb3e9f016de957509ea19550d7812c37a7951f2699afad88de253501a54bf317`.
Addresses below describe this image and this sample, never model constants.

With `SCTLR=0x00c5387d`, `TTBCR=2` (short descriptors), and
`TTBR1=0x40940018`, the high VA uses TTBR1:

| Read | Address | Descriptor / result |
|---|---|---|
| L1 index `VA >> 20` | `0x40943b4c` | `0x408f2001`, coarse table |
| L2 index `(VA >> 12) & 255` | `0x408f2234` | `0x41014013`, small page |
| Polled word | physical `0x41014018` | `1` |

Thus `0xed38d018` is **DRAM**, not SGX MMIO. Physical allocation changed between
paused samples, reinforcing that a fixed physical address is unsuitable.
`DACR=1` and `CONTEXTIDR=0x14` were also captured. Reading the same VA directly
and through physical-memory mode returned the same value; physical-memory mode
was disabled before continuing execution.

The driver object's `+0x36c` points to mapped device registers (`0xed36e000`);
`+0x3f8` points to an allocated-memory descriptor (`0xc1039500`), and `+0x3fc`
to its CPU mapping (`0xed38d000`). Descriptor `+4` contains GPU VA `0x19002000`.
The stock allocation path at `0xc069462a` allocates size `0x110`, with 4 KiB
alignment, then obtains the descriptor's CPU address and stores it at `+0x3fc`.
This resolves the older research's uncertainty about a supposed second MMIO
window: at least this driver field is a shared allocation.

## Producer and bring-up contract

The consumer sets shared `+0x18` bit 0 at `0xc0695f46`, clears shared `+0x24`
and `+0x28`, calls hardware initialization at `0xc0695bf4`, and waits for that
bit to clear. No timeout or scheduler call occurs in the observed tight loop.
Initialization resets/configures SGX BIF registers and invokes `0xc0694de4`,
which sets up GPU-addressed buffers and firmware launch state. In that setup,
`+0xa0c` receives a USE code-base value, `+0xac4` a GPU buffer address,
`+0xa68/+0xa6c/+0xa70/+0xa58` execution parameters, and `+0xab8` the PDS base.
The initialization path enables interrupt event bit `0x4000` at `+0x130`.
Earlier reset code supplies the GPU page-directory physical address to `+0xc84`.
The next operation after a successful shared-memory acknowledgment kicks the
GPU at register `+0xac8`.

These facts support GPU firmware as the expected producer, but the exact Apple
shared-structure field names and USSE instruction sequence that acknowledges
`+0x18` are **not decoded yet**. They must not be inferred from offsets in a
newer Linux DDK. The public Imagination
[SGX microkernel interface](https://github.com/martinezjavier/omap3-sgx-ddk-linux/blob/master/eurasia_km/services4/include/sgx_mkif_km.h)
documents the general host/shared-memory arrangement, but its conditional
layout does not identify this older Apple structure.

A faithful next step is to trace the BIF directory and GPU buffer mappings,
identify the firmware entry and acknowledgment store, and implement the
required GPU execution/memory/event behavior from that contract. That must
include correct faults, reset, IRQ acknowledgment and subsequent command
completion, with addresses derived from programmed hardware state. Clearing
this RAM bit in the host would bypass the contract while leaving the firmware
and rendering path unimplemented. ANGLE behind the existing guest GLES
transport would likewise not execute this stock SGX firmware.

## Evidence and reproduction

The isolated run used `tests/ipad1/restore-coldboot.py --gdb --timeout 300`,
private overlay/NOR copies, a private USB endpoint, and no guest additions.
Evidence is retained under `/private/tmp/ltm-sgx-mapping`: `inputs.json`,
`qemu-command.json`, `sgx-mapping.json`, serial/USB logs and `result.json`.
The debugger queried target XML for the ARM system-register numbering,
read TTBR1/TTBCR/SCTLR/DACR, walked short descriptors using
`Qqemu.PhyMemMode:1`, restored virtual mode, and resumed. Future samples must
walk their own tables; the addresses in this note are not reusable targets.
