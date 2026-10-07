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

The old-compatible dirty bitmap remains a separate file but now also uses a
QEMU BlockBackend. Its live ownership map is RAM only, preventing OS mmap
writeback from exposing ownership before the associated page data is durable.
Every clean stop and migration pre-save flushes all page backends first, then
writes and flushes pending ownership. Any page flush failure prevents ownership
publication; any publication error latches save/resume refusal. An unchanged
ownership map is not rewritten after migration deactivates source backends.
This is ordered durable publication under an explicit volatile-cache contract:
it does not certify that every acknowledged flash program survives a host
crash, atomic guest operations, or atomic updates to already-owned pages.
A crash may discard unpublished first-owner writes; it cannot make the bitmap
select their incomplete data. Already-owned pages retain ordinary device
power-loss/torn-write behavior. Host generation transactions remain separate.
This does not remove generated plaintext/FTL compatibility. The upstream NAND model supplies the correct bit semantics,
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
VMState version 5 now serializes both through upstream QEMU's GTree serializer.
Incoming state clears derived overlay indexes, validates physical coordinates
and markers, and refuses differing startup read/write modes. Version 4 streams
were certified empty and remain accepted; versions 1–3 remain refused because
they silently omitted unvalidated maps. This does not remove generated-layout
FTL relocation or make a RAM snapshot interchangeable with another flash generation.

Real `ipod-fmss-test` qtests initialize the actual N72 board without executing
firmware. Four checks cover empty migration, mismatched-mode refusal, physical
program/read after save/load/reset, and generated pages with no disk destination
that exist only in RAM. The latter recover data and spare bytes from the stream
and return to their pristine cold-boot view after reset. Both physical and
generated modes preserve pages on two chip selects. Evidence:
`/private/tmp/ltm-fmss-map-final-qtest2.log`. The physical fixture removes its
disk erase marker after saving and uses a contrasting backing page to prove the
RAM-owned marker restores (including direct key zero).

The native 7E18 acceptance saves with a live Harness GL scene and written guest
file, then resumes a fresh QEMU process against the same flash overlay. Agent
rekey, file content, USB re-enumeration and lockdown pairing, host clock, restored
GL contexts, fixture pixels and continuing frame presentation pass. Evidence:
`/private/tmp/ltm-fmss-tree-native-snapshot2.log`. A separate native run restores
while stereo PCM is playing and then fetches HTTP through Wi-Fi. Restored output
has the expected 440/880 Hz channel frequencies for at least two seconds; HTTP
returns the fixture bytes before and after restore. Evidence:
`/private/tmp/ltm-fmss-tree-audio-network-snapshot.log`. This probes new HTTP
requests, not preservation of an already-open host TCP socket. The final default
eight-check iPod tier also passes, including fsck, app launch, audio and cold
file persistence: `/private/tmp/ltm-fmss-tree-final-native-regress.log`.
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


## Ordered ownership acceptance

The actual H2FMI qtest forks an isolated driver, programs real physical zero,
then SIGKILLs its own guest without any stop or cleanup callback. The old-format
bitmap remains clear, and cold reopen reads the immutable base. A subsequent
program followed by real migration pre-save publishes the bit before clean
exit, and another cold reopen reads zero. This proves ordered publication for
first-owner pages, not crash-atomic flash operations. The host failure unit
injects page flush, bitmap write, and bitmap flush failures; page flush failure
publishes no ownership, and each failure reaches the GUI save/resume guard.

Evidence: actual H2FMI **7/7 PASS** at
`/private/tmp/ltm-ownership-final-model3.log`, registered model tier **4/4 PASS**
at `/private/tmp/ltm-ownership-final-modelgate2.log`, and standalone N45 touch
qtest PASS at `/private/tmp/ltm-ownership-touch-final.log`. Intermediate failures
remain at `/private/tmp/ltm-ownership-snapshot.log` (inactive-backend rewrite)
and `/private/tmp/ltm-ownership-final-model.log` (nonempty FIFO migration).

The latter exposed H2FMI's fixed FIFO arrays wrongly declared with pointer
VMState macros. Four fields now use bounded fixed-offset variable buffers,
reusing upstream buffer serialization. Incoming lengths are rejected before
buffer access, while v1 wire bytes are preserved. The actual save/load test
retains nonempty data/meta and verifies their bytes on arrival. Empty-state
migration passing had not established this contract.

Prepared native ordering build boot + 70001-byte clean shutdown/reboot
persistence **2/2 PASS** (76.8s): `/private/tmp/ltm-nand-ownership-native`.
Fresh explicit-v2 stock erase restore **PASS**:
`/private/tmp/ltm-nand-ownership-restore`. These runs preceded the narrow
pending-transition/serialization completion. Final snapshot-fixed binary
persistence **PASS** (48.4s), but that concurrent boot check failed at unlock
with black display/no NAND error:
`/private/tmp/ltm-nand-ownership-final-native`. The failure is preserved and an
isolated traced boot replay is evaluated separately; it is not counted as full
native acceptance. Initial file/format-marker directory metadata is synced
before any ownership can reference those fixed filenames.

The isolated final binary boot/unlock replay **PASS** (45.6s), with actual
MT_TRACE frame reads, at `/private/tmp/ltm-nand-ownership-boot-isolated`.
Together with final native persistence this establishes the bounded final
contracts in separate runs; the concurrent unlock failure remains unexplained
and is not erased from the acceptance record. A directory-fsync initialization
barrier and pending-only publication leave no mmap bitmap writeback path.
