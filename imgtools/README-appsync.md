# AppSync-equivalent: install + launch decrypted apps over USB

Make `ideviceinstaller install <decrypted .ipa>` produce an app that actually
**installs over USB and launches** on the iPod Touch 2G emulator (iPhone OS
2.1.1), by defeating the two userspace code-signing gates offline. This is the
capability the period jailbreak tweak *AppSync* provided, done as deterministic
binary patches on a NAND COW clone instead of by installing a 2008 package.

## One command

```
cp -Rc /Users/shg/Developer/qemu-ios-files/nand-noidlelock <clone>     # COW clone
imgtools/patch_codesign_gate.py --nand <clone>                          # applies both patches
```

`patch_codesign_gate.py` reassembles the volume, applies both patches, `ldid -S`
re-signs each binary, runs `fsck_hfs`, and writes only the changed pages back. It
**refuses the golden image**. Boot the clone, pair, then
`ideviceinstaller install <decrypted .ipa>`; the icon appears and launches.

## The two gates (independent)

| | Blocker 2 — install | Blocker 3 — launch |
|---|---|---|
| Symptom | `ApplicationVerificationFailed` at `VerifyingApplication (40%)` | `SBAppCannotBeOpenedAlertItem`: "…cannot be opened" |
| Decider | `MobileInstallation._MobileInstallationInstall` → imported `_MISValidateSignature` (libmis) | `-[SBIconController launchIcon:]` → `-[SBApplication applicationSignatureState]` |
| Patched binary | `/usr/lib/libmis.dylib` | `/System/Library/CoreServices/SpringBoard.app/SpringBoard` |
| Patch | `MISValidateSignature` (thumb, va `0x33a27c58`): `movs r0,#0; bx lr` → returns success | `applicationSignatureState` (arm, va `0x00027b44`): `mov r0,#2; bx lr` → always "trusted" (2) |

They are genuinely independent: a real installd install (proper registration)
still would not launch until blocker 3 is also cleared. Proven by A/B — the same
decrypted binary launches from `/Applications` (a system app ⇒ signature state 2)
but not from `/var/mobile/Applications` (user app, no trusted signer identity ⇒
state 0).

`applicationSignatureState` is computed **purely from userspace on-disk/registration
state** (the `_signerIdentity` ivar from MobileInstallation parsing the signature,
`MobileInstallationCopySignerIdentityTrust`, and the system-vs-user flag) — it never
consults the kernel. A kernel codesign-disable would let a binary *run* once
launched but would **not** satisfy this launch check, so the SpringBoard patch is
independently necessary.

## Why re-signing (`ldid -S`) is required

`amfi_allow_any_signature=1` forgives an **invalid top-level signature**, but the
kernel still hash-checks each **code page** on fault-in against the CodeDirectory.
A byte patch changes a page whose CD hash no longer matches → the kernel faults it
in as `*** INVALID PAGE ***` and the process dies (an unpatched-but-not-resigned
attempt did exactly this at `0x333ab000`). `ldid -S` recomputes the CD page hashes
over the patched bytes; `imgtools/cdverify.py` confirms every slot matches. The
top-level signature becomes ad-hoc/invalid, which the boot-arg forgives. This is
also why Clutch-decrypted apps run: Clutch recomputes their page hashes.

## Scope / limits

- Works on **decrypted (`cryptid 0`)** bundles. Verify `DTSDKName` starts
  `iphoneos2.` and avoid apps linking `OpenGLES.framework` (MBX GPU unemulated).
- Does **NOT** help encrypted App Store apps: still FairPlay-encrypted (`cryptid 1`),
  the kernel refuses to exec their mismatched `__TEXT` pages regardless of these
  patches.
- Verified end to end: decrypted **Obama '08** installed via `ideviceinstaller`
  (`Install: Complete`) and launched to its rendered main menu, zero `INVALID PAGE`.

## Tools

- `patch_codesign_gate.py` — the one entry point (applies both, re-signs, refuses golden).
- `patch_libmis.py`, `patch_springboard.py` — the individual byte patches.
- `cdverify.py` — verify a Mach-O's CodeDirectory page hashes match its bytes.
- `editimg.py` — reassemble/mount/patch/fsck/write-back a NAND page image (used internally).

A DYLD_INSERT interpose dylib was tried first for blocker 2, to avoid modifying
any Apple binary. It does **not** work here: the only process that does the
install validation (`mobile_installation_proxy`) is a lockdown *service* child
with no launchd plist of its own, so the insert has to go on lockdownd itself —
and a failed insert aborts lockdownd, taking pairing and every service down (the
observed failure: SpringBoard never connects to lockdown). The re-signed libmis
patch is what actually works and is what `patch_codesign_gate.py` uses. That
attempt is preserved on the `appsync-offline-patch` branch, not shipped here.

The verified launch — decrypted Obama '08 installed over USB and opened to its
rendered main menu — is `docs/screenshots/appsync-obama08-launched.png`.
