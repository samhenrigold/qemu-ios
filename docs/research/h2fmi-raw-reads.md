# H2FMI physical reads during blank-device restore

## Evidence and contract

A stock 7B500 restore ramdisk, launched through emulated SecureROM/USB with
`tests/ipad1/restore-smoke.py --erase --blank-nand`, initially failed its bad-block
table scan. The fixture creates geometry and eight sparse physical chip files;
it does not seed a BBT, FTL, filesystem, or guest instructions.

IOP ring traces showed normal operation 8 reading blank pages 0/1/2 successfully
(status 2), followed by raw operation 5 at physical page 125 returning
`0x80000016`. H2FMI traces and the stock ARM7 firmware establish three independent
controller contracts:

1. The NAND `0x00`/`0x30` command latches a new page. The subsequent FMI_CONTROL
   write can remain 3 from the previous read; a control edge is not required.
2. Raw FORMAT `0x40004` sends the physical data plus spare through DATA. Normal
   FORMAT `0x14540004` instead sends 4096 data bytes and 10 extracted metadata
   bytes through separate FIFOs. Raw erased bytes are `0xff` and require no ECC.
3. Raw transfer has independently acknowledged phases. The IOP selects the data
   phase, waits for FMI_STATUS bit 1, and writes bit 1 back to clear it. It then
   selects the spare phase (observed FORMAT `0x8001`), writes CONTROL 3, and waits
   for a fresh bit 1 completion. An unchanged CONTROL/FORMAT acknowledgment must
   neither reload the page nor signal a new completion.

The observed second phase contains 128 spare bytes. The model uses the generic
raw FORMAT phase transition, not a special comparison with that observed value.
The existing byte stream is retained across phases even when a prepared CDMA
chain already drained its bytes before CONTROL is written.

For reproducibility, the relevant stock ARM7 raw routine is at offset `0x474c`;
data FORMAT/CONTROL are written at `0x4808`/`0x4818`, spare FORMAT/CONTROL at
`0x48f0`/`0x48fc`, and both call status polling routine `0x2b74` with offset `0xc`,
mask 2, expected value 2. That routine acknowledges completion at `0x2c50`.
These are analysis offsets, not emulator address constants.

## Incremental validation

The pending-command fix moved the failure from the data phase to the spare
phase. Streaming physical spare bytes then allowed raw operation 5 to succeed,
but the absent second DONE incurred about 100 ms of polling per page. The phase
completion fix removed that delay: the scan reached block 3084 at about 32 virtual
seconds, versus block 498 at about 118 seconds before it.

`tests/qtest/ipad1-h2fmi-test.c` uses upstream libqtest against the real ipad1
machine with no executing firmware. It covers normal-to-raw reads, physical
spare bytes, W1C completion for both phases, and no phantom refill or completion
on unchanged CONTROL. The H2FMI sanitizer unit test also passes. The explicit
`tests/gate.sh --models` tier passes both this qtest and the PMGR clock/timer/reset
qtests (2 passed, 0 failed).

Pending-read/phase state is held by the controller and migrated in optional
`read-pending` subsection version 2. The original FIFO stream stays version 1;
old snapshots default to no pending command and the restored FORMAT as their
already completed phase.

Fresh K48 guest regression after the final hardware change passes both boot and
persistence: a 70001-byte marker survives guest-confirmed shutdown and reboot on
the same overlay (84.2 seconds total). The boot check verifies lock/unlock and
that the GL bridge refuses no calls; it has no reference-image comparison.

The blank restore progressed from initial raw scan into physical page/cache
programs (IOP operation 9, status 1) by about 116 virtual seconds. This alone does
not prove a complete blank-device restore or a bootable restored filesystem.
The guest subsequently created its partition map and filesystems, connected to
ASR, validated the filesystem, and started receiving it. The first 300-second diagnostic reached ASR 100% and "Verifying restore (14)"
before its bound expired. A fresh geometry-only 900-second run follows to
measure full completion; this is the runner's normal default bound, justified
by sustained guest progress rather than by hiding the original polling gap.

## Completed stock restore

The fresh 900-second run completed: stock `idevicerestore` exited 0 after
"Flashing firmware (18)", "Finalizing NAND epoch update (32)", unmounting the
filesystems, and "Status: Restore Finished" / "DONE". The runner reported
`PASS: blank-NAND erase restore`. Its output contains the actual physical chip
files and restored NOR; no synthetic FTL or guest additions were supplied.

An independent SecureROM cold boot uses an APFS clone of those artifacts and
the same identity and per-IPSW catalog GID data, with development fuse policy
for unpersonalized stock firmware. This separates cold-boot evidence from the
restore tool's completion message.
