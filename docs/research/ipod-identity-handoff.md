# N72 factory identity and boot argument boundary

The October 1 candidate removes the direct-iBoot empty-string literal redirect.
It does not remove the remaining Bluetooth path rewrite or the host-supplied
AMFI handoff-buffer writer, and therefore does not certify an unmodified boot.

## Measured cause

A private 7E18 NOR contains the generated serial, model and region in SysCfg.
A debugger control with `boot-args=` stops at stock SysCfg reads: iBoot reads
`SrNm` successfully into both its USB serial buffer and the 32-byte DeviceTree
root property. NOR layout and SPI transfer are not the fault.

With the old early argument injector enabled, the same stock root lookup instead
receives the staged command string as its node path. The selected empty-string
literal is shared by command-line construction and DeviceTree root lookups.
Changing its pointer skips root population. A read-only capture at the kernel's
Mach-O entry shows serial/model/region all zero before any kernel code executes.

The running guest agrees: `IODeviceTree:/` contains a zero serial and
`IOService:/` publishes an empty `IOPlatformSerialNumber`. Lockdown's UDID is
SHA1 of the empty serial plus Wi-Fi and Bluetooth addresses, rather than the
prepared identity. This is not a lockdown cache or an artwork importer bug.

## Candidate and scope

Remove the shared-literal mutation, staging allocation and now-unused finder/API.
Use zero initial delay and 1 ms polling while searching for the handoff buffer,
then the existing configured refresh interval after discovery. Bound the search
by the configured write-count times refresh interval. The stock early UART
console gate passes with this two-phase writer; slower initial timing loses that
output even when home and media pass. This remains compatibility timing, not a
hardware or firmware-consumption guarantee for every build.

Keep the existing repeated boot_args buffer writer and the legacy SecureROM
command-line data write; those remain explicit provisioning compatibility.
Native 7E18 now reports the generated serial and expected UDID. The media cold
reopen gate checks serial, UDID and both MAC addresses on each boot, alongside
exact full tags, one row after retry, and artwork decoded by stock MediaPlayer.

The native Bluetooth-only experiment also establishes that a correct HCI
ReadBDADDR result alone does not replace the iBoot UART node rewrite: without
that rewrite, lockdown receives a zero Bluetooth address before any HCI command.
That gap is retained rather than declaring a fixed-address HCI replacement done.

## Evidence retained outside the product

- `/private/tmp/ltm-n72-syscfg-gdb-all/result.json`: successful stock factory reads.
- `/private/tmp/ltm-n72-serial-handoff/`: old injector, unchanged stock code and
  actual kernel-entry DeviceTree with missing root identity.
- `/private/tmp/ltm-n72-identity-ioreg/`: old running IORegistry/lockdown control.
- `/private/tmp/ltm-n72-identity-no-literal/`: restored native IORegistry identity.
- `/private/tmp/ltm-n72-identity-media-phased/`: import and hard-reopen identity,
  MusicLibrary tags and stock-decoded cover image.

Debugger addresses and scratch programs are research observations only. No
per-build guest address or new instruction patch is introduced into the models.
The ramdisk one-shot now stages its command line through the existing host
debugger handoff together with RAMDisk/topOfKernelData; it disables emulator
argument injection entirely. It must not race a firmware timer or alter the
empty root path.

Other firmware generations need separate native qualification; an epoch finder passing against their images proves discovery, not boot behavior.

Production-board handoff qtests pass 3/3, including a signature spanning a
scanner window, exact refresh count/cadence, reset pointer invalidation, disabled
arguments and search expiry. All seven model suites and 109 host checks pass.
The initial 4.2.1 one-shot control uses an obsolete prepared base whose stock
iBoot rejects its VFL checksum before kernel entry; it is not handoff evidence.

A fresh 8C148 fixture built by the current Swift preparer completes the host-owned
Update ramdisk keybag one-shot (6 s, effaceable NOR changed, 7 NAND pages folded).
Its existing ad hoc helper is source 9d2d3c2f0b, with argument injection explicitly
disabled; it is not evidence of a rebuilt universal 7db4df5b76 package. A subsequent
cold boot with the current emulator passes stock BSD mount logging and all four
factory identity readbacks. The optional 3.1.3-linked IORegistry scratch probe
fails to execute on 4.2.1; it is not counted as a 4.x guest-tool regression pass.

- `/private/tmp/ltm-n72-4-current-prepared3.log`: current production Swift create.
- `/private/tmp/ltm-n72-identity-ios4/`: current emulator 4.x identity/console.
- `/private/tmp/ltm-n72-identity-console-phased/`: native 3.1.3 early UART gate.
