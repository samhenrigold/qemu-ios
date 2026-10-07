# Matched-guest iPad audio references

The iPad audio gate no longer depends on a developer's mounted 7B500 rootfs.
It reads the stock sound resources from the exact tested guest through the
existing agent VFS operation, before shutdown. Standalone, default regression
and snapshot audio share this capture function. A legacy image without an
agent needs an explicit matching `--sound-reference-root`; another firmware's
resources are never silently substituted. Native paths remain declared sound
selectors, not guest instruction addresses.

The standalone tool now uses the existing Boot owner, private NAND overlay and
NOR copy, rather than duplicating QEMU launch, NAND cloning and child cleanup.
`--keep` retains sound inputs as well as the WAV and serial log. Apple resources
remain private test inputs, not release payloads.

Real 7B500 validation: boot power, unlock, lock and unlock correlate at
0.92, 0.86, 0.90 and 0.86 against their stock resources. Both the standalone
and production regression entry points pass. The nine-check iPad gate also
passes boot/unlock/lock, qualified GL Home comparison, USB, AFC, clean two-boot
persistence, Wi-Fi, early join and guest HTTP. This does not certify every
firmware's sound selector or stereo channel order (this oracle samples one
channel; the separate iPod tone gate checks distinct left/right frequencies).

Evidence:

- `/private/tmp/ltm-k48-physical-card-default-regress.log`: retained failure at
  the missing host reference, eight other checks pass; not a hardware failure.
- `/private/tmp/ltm-k48-physical-card-audio-regress.log`: repaired guest oracle.
- `/private/tmp/ltm-k48-physical-card-audio-standalone.log`: shared Boot owner.
- `/private/tmp/ltm-k48-physical-card-final-regress.log`: full 9/9, no skips.
