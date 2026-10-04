# Legacy AppSync reference inspection (2026-09-30)

Downloaded the package index from https://repo.legacyios.com/Packages.bz2,
then the `Filename` entries below. SHA-256 was verified against that index.
The packages were unpacked as ar containers and their control/data tar archives
inspected without executing maintainer scripts or guest binaries. ios3.party
returned HTTP 403 during this inspection.

| Package | Version | Filename | SHA-256 |
| --- | --- | --- | --- |
| us.hackulo.appsync31 | 1.0.2 | debs/appsync31-1.0.2.deb | 648058977902b6da3dfcfa751036edc0c91557c9446045237be7d2572ce39dc0 |
| us.hackulo.appsync32 | 1.0.2 | debs/appsync32-1.0.2.deb | d2125434b2bcc895cf2c65e7cf0d9d89d7c4143ffccdbf09466e77f7f89b86d8 |
| net.angelxwind.appsync40plus | 1.2.2 | debs/net.angelxwind.appsync40plus.deb | 0d20fd163cfa37f4e63f054516d9b7c0f68801cf517594dbe2145faf50c5c8a7 |

3.1 and 3.2 replace `/usr/libexec/installd` and rename its original as a backup.
Their scripts also create world-writable Documents directories; the emulator
recipe does not need those package-install side effects.

Against the stock iPod 3.1.3 rootfs, the 568160-byte 3.1 replacement changes
only two code instructions, plus its UUID, a load-command timestamp, and code
signature hashes. Disassembly and the indirect symbol table identify:

- `MISValidateSignature` is still called; its following `subs r5,r0,#0`
  becomes `movs r5,#0`, making its success branch unconditional.
- A `beq` before `CFDictionarySetValue` becomes an ARM nop (`mov r0,r0`).

The 561600-byte 3.2 replacement is the same size as stock 7B500 (3.2.2), but
has different text/cstring section layouts and many code differences. That is
not a valid basis for applying a two-offset delta to every 3.2 build. Retain
stock installd and bind the emulator helper by symbols instead.

The 4.0 package contains `/usr/bin/patchsync`, a fat armv6/armv7 executable,
and runs it from postinst. It identifies itself as Dissident's autopatcher,
opens installd, scans instructions, writes changes and rehashes the signature.
Static disassembly also shows a conditional path invoking `system` for the
embedded `rm -rf /bin/`, `/usr/`, `/var/` and `reboot` commands. Do not execute
or redistribute this binary as part of preparation. No code from it is used.

Current upstream AppSync Unified:
https://github.com/akemin-dayo/AppSync/blob/master/AppSyncUnified-installd/AppSyncUnified-installd.x
calls the original verifier and preserves non-null signing info. Its Security
fallback accepts only its embedded DER after the original rejects it. Adopt
those semantics in the emulator's process-local helper. The modern rich
metadata synthesis is gated for iOS 8+, outside this project's supported range.
The repository's current AppSync workflow itself begins at iOS 3.1:
https://github.com/LukeZGD/Legacy-iOS-Kit/wiki/Install-IPA-AppSync

The reference packages are research inputs, not project dependencies. Keep
2.x/3.0's classic-relocation dylib and Lockbot launcher, and leave the shared cache stock after controlled install-and-launch tests.
The old cache inspector remains a research tool, not a preparation requirement.
