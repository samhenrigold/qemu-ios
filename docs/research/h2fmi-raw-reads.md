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
`tests/gate.sh --models` tier passes the H2FMI qtest, PMGR clock/timer/reset
qtests, and CDMA/AES/SHA qtests (3 passed, 0 failed).

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
ASR, validated the filesystem, and started receiving it. The first 300-second
diagnostic reached ASR 100% and "Verifying restore (14)" before its bound
expired. The subsequent fresh geometry-only run completed within the normal
900-second bound, justified by sustained guest progress rather than by hiding
the original polling gap.

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

## Independent cold-boot boundary

The first screenshot was the stock iBoot recovery graphic, not a userland
activation screen. A brightness-only boot-smoke success was therefore
insufficient. The restored latest NOR environment bank held `auto-boot=false`.
Stock `irecovery --normal` over emulator-only USB changed the private clone's
environment; the next SecureROM cold boot initialized HFS and loaded and
uncompressed the kernelcache. Production iBoot passed empty boot arguments;
even stock recovery commands setting verbose boot arguments persisted in NOR
but were not passed to the kernel. Missing serial launchd markers alone cannot
classify that quiet boot as a kernel failure.

`tests/ipad1/restore-coldboot.py` now judges actual stock lockdown identity
(ProductType, ProductVersion and serial number). Normal cold boot has the
canonical direct QEMU-to-usbmuxd connection, with no recovery poller accessing
EP0. Its optional recovery-exit path uses a private transport adapter and stops
recovery queries after normal-mode handoff. Each run preserves its own NOR
clone, NAND overlay, input hashes and structured result. It offers no guest
package, activation hook, filesystem seed or FTL repair.

Canonical cold boot exposed a separate host USB control-transfer bug: after
128 of 149 configuration bytes, the daemon treated 30 NAKs as completion and
then consumed stale remaining bytes as another descriptor header. Isolated
usbmuxd commit `a5cc2b0` uses 64-byte EP0 packets, actual short-packet/ZLP
termination, and the full control timeout for NAK flow control. The real daemon
TCP fake-guest test passes a 149-byte configuration interrupted by 60 NAKs, a
short descriptor, and a 64-byte descriptor plus ZLP. The unchanged daemon
fails that regression at 128/149 bytes. Existing prepared-device USB identity,
AFC size-boundary transfers, and shutdown/reboot persistence all pass with the
new daemon.

With that host fix, the stock restored kernel enumerates PID 05ac:129a and
complete configuration descriptors; AppleUSBDeviceMux establishes a loopback
session to port 62078. That diagnostic is not a lockdownd RPC response. The
stock `ideviceinfo -s` QueryType request had not received its application reply
within the initial bounded probes. Full restored userland, activation and AFC
persistence therefore remain unproven; they must not be inferred from the
restore completion marker, recovery brightness, or a kernel mux session.

A second normal cold run allowed 300 seconds and still failed the identity
judge. The subsequent LLDEBUG wire run shows successful TCP SYN/SYN-ACK followed
by the host's 288-byte QueryType application payload, but no guest TCP ACK or
application reply before the client timeout. USB accepting that OUT and
AppleUSBDeviceMux printing NewSession are insufficient to prove delivery to a
working daemon.

A read-only gdb sample found CPU0 at `0xc0695f6a` in the IMGSGX535 driver:
`ldr r3,[r2,#0x18]; ands r5,r3,#1; bne` with `r2=0xed38d000` and the busy bit set.
This is a native GPU hardware barrier: the board has no SGX model. The prepared
direct-kernel path explicitly sets `arm-io/sgx`'s `compatible` property to `none`
in `imgtools/ipad1_kboot.py:fill_dt`, preventing IMGSGX535 from matching. The
prepared real-iBoot DeviceTree helper in `imgtools/ipad1_gid.py` makes the same
edit. Prepared-device regressions therefore do not validate stock SGX startup.

The available gdb dumps contain core registers and instructions but no MMU
translation/page-table data. The polled VA remains unmapped: it may address an
MMIO register or GPU-shared RAM. Resolving that mapping is required before any
controller change; neither the driver's busy loop nor the missing native SGX
model justifies clearing a guest buffer bit or inventing a completion. No such
change was made.
