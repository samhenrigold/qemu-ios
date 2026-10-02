# Measured MBX fill execution

The experimental property `-global driver=ipodtouch.mbx,property=x-2d-fill,value=on` selects a
strict command consumer, default off. It rejects `IT_MBX_COMPLETE` and
`IT_MBX_RAM`, does not synthesize startup/EVM/context acknowledgements, and
therefore is not presently a usable stock-device boot mode. Default behavior
and the existing GLES compatibility path remain unchanged.

The narrow decoder derives from MIT-licensed S5LBox commit
`6f203ba550b49afadee008c7eb55373a838eed33`, `core/src/soc/mbx.c`. The copyright
and full permission notice are retained in `licenses/S5LBox-MIT.txt`.
Independently captured N72 7E18 command words and the eight-root raw-page GART
agree with this decoder in prior sanitizer replay. The old RAM capture was
after a stall: it does not establish live pixel accuracy or EVM semantics.
See the bounded prior receipts in
`/Users/shg/Developer/ltm-evidence/mbx-reuse-2026-10-01`.

The operation admitted here is exactly the captured full-screen opaque-black
BGRA8 fill: 320x480, 1280-byte stride, one freshly written 16-word packet,
`a0060500` header, fixed aperture ring+0 `f0000000` trigger, and raw page-aligned
GART entries. These are restrictions on experimental coverage, not universal
MBX geometry. New colors, rectangles, blends, copies and batches are rejected.

All 150 translated pages are validated as ordinary writable RAM before any
pixel write. Their physical addresses are frozen before commit, so writes
which alias the page table cannot change later translations. Pixel writes use
QEMU AddressSpace writes and therefore normal dirty tracking/observers.
Malformed packets, old unwritten tail words and missing/ROM/MMIO targets are
refused without output. An unexpected bus error during commit may leave
partial output; it never raises completion. Successful execution alone posts
2D-sync `0x400`, subject to the existing mask, observational STATUS and W1C.
Register copies, startup polls and timer expiry do not qualify completion.

The synchronous consumer retains GART roots, ring contents, packet freshness
and pending packet count across migration. Option mismatches and old streams
without this state cannot restore a strict consumer. Reset clears all this
state and the interrupt. No host pointer, guest address patch or timer is
saved as command state.

Tests:

- `tests/ipod/test_mbx_fill.py`: source decoder under ASan/UBSan, exact output
  on discontiguous synthetic pages; malformed/late-hole/MMIO/batch refusals.
- `tests/ipod/test_mbx_fill_handler.py`: actual production handlers under
  ASan/UBSan, exact output and IRQ/mask/W1C, old-tail refusal, late invalid PTE,
  injected DMA failure withholding completion, reset and unsupported startup.
- `tests/qtest/ipod-mbx-status-test.c`: actual production board DMA/pixels,
  pending IRQ/W1C, refusal/reset and staged command migration; the existing
  four default-mode IRQ tests remain separate.

Native qualification remains pending root scheduling. A pre-submit capture
can test this operation's real packet/RAM, but cannot authorize inventing the
preceding EVM pool metadata effects. A strict native boot expected to stop at
EVM startup is an honest investigation gate, not renderer qualification.

## Independent MMU contract refinement (2026-10-02)

NXP's primary [MCIMX31RM, revision 2.4](https://www.nxp.com/docs/en/reference-manual/MCIMX31RM.pdf),
section 46.3.2.2 (pages 46-4/46-5), documents the MBX family MMU separately
from the event/parameter allocator. Its 32 MB GPU address space uses 8192
32-bit entries for 4 KB pages; eight static registers locate the table pages.
It bypasses translation after reset and when disabled. Enable initializes a
translation cache and exposes readiness; the manual describes approximate
latency but does not establish N72's exact timing. This corroborates the
captured table topology and identifies a flaw in the initial fill candidate:
it translated through the GART even when the guest had never enabled the MMU.
The original six-case board fixture consequently omitted the enable write.
The scoped correction now uses the MMU control request to choose GART
translation or physical bypass. In strict mode the ready bit follows the
enable request and cannot be forged by a guest write; reset clears it. The
legacy default remains unchanged. Cache timing is still immediate. Production
leaf and handler sanitizer suites verify disabled translation refusal, valid
physical bypass despite invalid roots, control/readiness and reset. The rebuilt
actual-board suite passed all six cases, including disabled-MMU refusal and
staged migration, in `/private/tmp/ltm-next-mbx-mmu-qtest.log`; the corresponding
build receipt is `/private/tmp/ltm-next-mbx-mmu-qtest-build.log`. This qualifies
model behavior, not native EVM startup or stock rendering.

This primary manual does not provide EVM free-list entry layouts or startup
DMA effects. The pinned public GPL MBX driver repository's buffer/device/service
wrappers were also inspected: they manage host mappings and service interfaces,
and do not supply an EVM allocator implementation. Stock 7E18's examined driver
references never consume the CPU table after allocation; they cannot establish
its hardware-generated links. Neither source justifies acknowledging startup.

Local primary-manual receipt: SHA256
`d3dd82216e6dc0e702fe410bb2d76f6cf63c32582cce22f03f02a40ab065ccac`,
25,909,049 bytes. Research files stay in private scratch; no vendor header or
manual content has been copied into emulator code.

## Live strict EVM observation

`/private/tmp/ltm-mbx-strict-current-startup` captures five stock 7E18
pre/post-init, pre/post-tag and first-poll boundaries. The owned guest was
reaped, canonical corrected NOR SHA256
`0e1d7be62ff5102a550545f4fc44b75afa52bb4508b8db7c61df39bd3d7de97e`
was unchanged, and the private suppression-free AES artifact retains the
current MBX/MMU objects (binary SHA256
`742448356ef29bc50aca6c356bc2456de60ad5d7d5c2c271870276129de9bbac`).

Actual CPU call arguments demonstrate writes 0x838=1 and 0x6d8=0x09000000.
Stock fields reserve object GPU page 0x100 and designate free endpoints
0x101..0x103. GPU table 0x15000 maps to physical 0x08cae000; the 16 KiB CPU
alias remains zero before and after both writes, and the first GPU page hash
also remains unchanged. GPU object base 0x100000 maps to 0x08c92000; its
reserved first-page hash matches the CPU alias at all boundaries, retaining
only the driver's header and six floating-point one constants.

The strict model's STATUS and mask remain zero at the first completion poll.
Its unsupported EVM register reads return zero: those reads do not imply the
guest wrote zero configuration. The capture establishes actual mapped inputs
and the first divergence, but an emulator with no allocator cannot supply the
silicon's link format, output writes or trigger ordering. It does not qualify
EVM startup or native rendering, and no completion acknowledgement is added.

That first live receipt did not include MMU control register 0x1020; installed
GART roots alone do not prove its enable state. Future bounded captures now
include control/readiness. The six-case board test establishes the measured
control behavior, but the omitted live field must not be retrospectively
claimed as native MMU qualification.
