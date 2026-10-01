# Physical NAND store formats and snapshot limits

## K48 representation

`geometry.json` may explicitly name `storage_format`:

- Missing or `legacy-zero-blank-v1` preserves generated-store compatibility.
  An all-zero stride is inferred erased and programming overwrites bytes.
  This representation cannot distinguish a hole from actual all-zero data.
- `nand-xor-ff-v2` stores each physical byte XOR `0xff`. A sparse zero hole
  therefore represents erased FF, while physical zero data is encoded FF.
  Programming applies physical `old & input`, as upstream QEMU NAND
  does: input ones leave existing bits unchanged, input zeros clear them.
  Only erase can restore a physical zero bit to one. Backend parameter errors
  expose NAND status bit 0 (failure), retaining it through polling until reset
  or the next operation. No chip program-count limits are invented. Erase returns a complete physical block to FF.

New geometry-only `restore-smoke.py --blank-nand --erase` fixtures use v2.
Existing prepared devices are not converted or silently interpreted differently.
The same format applies to base and overlay; v2 overlays require a matching
`storage-format` marker or an empty directory, and an existing marker mismatch
fails initialization. An untouched overlay page inherits the full base stride
before a partial program. File sizes must match geometry; existing nonempty
files are never resized. Geometry, chip ID, bus/chip/page bounds, and bitmap
rounding are checked before mapping or access.

VMState IOP version 2 carries a format certificate. Version 1 snapshots remain
loadable into legacy stores; v2 stores refuse streams without a matching
certificate. This only checks representation compatibility. Pairing a snapshot
with the correct NOR/NAND generation remains the host storage API's job.

K48 page data now uses QEMU raw BlockBackends: `blk_pread`, `blk_pwrite`,
`blk_pwrite_zeroes` with unmap permitted, and `blk_flush`. The adapter keeps the
existing filenames, explicit representation and dirty bitmaps; it adds no FTL.
`BDRV_O_NO_SHARE` permits concurrent readers while refusing another writer or
resizer. The actual two-process model test checks writer refusal. QEMU owns
platform file I/O, zero guarantees and backend locking instead of page-data
mmap, direct stores and platform-specific punch-hole code. Read/program/erase
host errors latch storage failure and return device error; stop flushes backends
before syncing bitmap ownership. Direct QMP migration additionally flushes and
refuses known failed storage. Device finalization releases backends/maps/handler.

The dirty bitmap still uses mmap and remains a separate file. This is not a
cross-file crash-consistency protocol or removal of generated plaintext/FTL
compatibility. The upstream NAND model supplies the correct bit semantics,
but its fixed chip geometries and backend layout do not directly fit the Apple
controller contract. N72 FMSS still needs a separately measured physical store
migration to replace its relocation compatibility; its current snapshot limit
is explicitly refused rather than concealed.

## Offline legacy conversion

Research command:

```sh
python3 imgtools/nand_store_convert.py OLD_NAND NEW_NAND --legacy-zero-pages erased
```

The caller must own the stopped device exclusively and reserve the destination.
The tool does not implement the production app's storage lease. It refuses an
existing destination, overlays, wrong sizes, or unsupported formats. Without an
explicit zero policy it rejects the first ambiguous legacy all-zero stride.
`erased` preserves the legacy interpretation; `data` preserves actual zero
bytes. Neither choice can reconstruct information missing from the old format.
Nonzero strides are losslessly transformed, including all-FF pages. The source
is retained; conversion uses a temporary destination and records source hashes
and policy in `conversion.json`. Source hashes are rechecked before publication.
Production conversion belongs in the shared device API with the same exclusive
lease and generation transaction as offline edits, rather than this Python CLI.

## N72 FMSS snapshots

FMSS `phys_pages` and `erased_blocks` hold authoritative physical read state
which its generated-layout compatibility files cannot fully reconstruct.
Previously snapshots silently omitted both maps. FMSS VMState version 4 now
refuses save when either map is nonempty, with a message directing the caller to
cold boot. Version 1–3 streams are refused because they do not certify empty
omitted state. Empty version 4 states can still save; ordinary boot and physical
writes are unchanged. This is an explicit limitation, not snapshot equivalence.

Real `ipod-fmss-test` qtests initialize the actual N72 board without executing
firmware, verify empty state migration, issue a physical page program through
FMSS MMIO, verify the persisted page exists, and verify migration fails.
`ipad1-h2fmi-test` tests actual K48 raw-read phases and physical program/erase:
actual zero data persists across cold reopen, a subsequent all-ones program leaves zero data unchanged, and erase reads back FF. The offline
converter test checks ambiguity, both policies, source preservation and refusal
without publishing incomplete output. These tests prove bounded contracts;
full restore and native lifecycle results must be recorded separately.

## Measured erase-address correction and native results

The first v2 restore exposed a controller bug hidden by legacy overwrite:
stock VFL programmed zero test pages, erased the candidate special block, then
wrote `DEVICEINFOBBT`. H2FMI interpreted every address as a read/program address
with two column bytes, so erasing intended block `0xfff` actually erased block
`0xe00`. Physical AND programming correctly retained the earlier zeros and VFL
verification failed. The stock erase transfer sends `ADDR0=0x7ff80`,
`ADDR1=0x7` (stale from a read), address count register `2` (three bytes).
NAND `0x60` has row-only addressing; the row starts at ADDR0 byte zero.
The corrected controller ignores stale ADDR1 for erase and selects row `0x7ff80`,
block `0xfff`. The real qtest explicitly includes stale ADDR1 to prevent relapse.

Fresh explicit-v2 stock 7B500 erase restore **PASS**:
`/private/tmp/ltm-nand-v2-erase-restore`, ending `Restore Finished` / `DONE`.
It started from geometry and sparse erased chips only. This demonstrates real
physical zero programming, erase and special-block verification through stock
VFL, filesystem restore and final flash/unmount. It does not establish restored
stock cold-boot/userland/GPU readiness, which has a separate SGX barrier.
Actual H2FMI qtests **2/2 PASS**, including invalid-row program failure status.
N72 prepared-device ordinary boot and persistence after guest-confirmed shutdown
and second boot **2/2 PASS** at `/private/tmp/ltm-fmss-native-regress` (108.8s).
Converter tests **3 PASS**. Existing legacy NAND and writeback fixtures pass.

## BlockBackend acceptance

Final upstream owner/permission ordering is tested with a real VM stream:
source save completes, a legacy-format destination refuses a v2 stream, and a
matching v2 destination loads it and performs a real program/read afterward.
Guest-owned backends attach before requesting incoming permissions; QEMU then
inactivates/reactivates them during migration normally. A storage I/O error is
latched, exposed to the host guard and refused by direct-QMP pre-save as well.
The bitmap/error callback unit remains host-error injection, not a pretend
emulator. The old extracted-memory NAND harness is removed; legacy and physical
contracts now run through the production board, controller and block layer.

Final results:

- Actual model gate **4 suites PASS**, no firmware prerequisites, including
  H2FMI **6 cases** and FMSS **2 cases**. Evidence:
  `/private/tmp/ltm-nand-block-final-models`.
- Native host gate **107 PASS, 27 declared manual SKIP**:
  `/private/tmp/ltm-nand-block-final-native-quick`. The sandboxed graphics/media
  run could not create native contexts; rerunning with native access passed.
- Prepared legacy K48 **boot/persistence 2/2 PASS**, preserving a 70001-byte file
  over guest-confirmed shutdown/reboot (54.4s):
  `/private/tmp/ltm-nand-block-final-native`.
- Fresh physical v2 stock erase restore **PASS**, final hardware build:
  `/private/tmp/ltm-nand-block-final-restore`, `Restore Finished` / `DONE`.
  The previous isolated BlockBackend run also passed. An earlier concurrent
  restore run ended with USB transport read error `-256` at filesystem creation,
  without a reported NAND error; its cause is not established, and its logs are
  retained at `/private/tmp/ltm-nand-block-restore`.

These are bounded acceptance results. They do not close the restored stock SGX
barrier, validate every IPSW, make cross-file runtime power-loss publication
atomic, or replace N72's generated-layout compatibility mapping. Existing
prepared formats remain deliberately supported rather than reinterpreted.
