# AppSync: IPSW-agnostic decrypted-app install/launch

One fat armv6+armv7 dylib, injected into `installd` and `SpringBoard` via
`DYLD_INSERT_LIBRARIES`, that lets a **decrypted** IPA install and launch on the
emulated legacy devices (iPod touch 2G / 3.1.3, iPad 1 / 3.2.2) without any
hand-found per-firmware byte offsets. It replaces the iPod's four fixed-offset
patches (`imgtools/README-appsync.md`) with symbol- and selector-bound hooks
that carry across firmware versions unchanged.

Encrypted FairPlay bundles are still out of scope; the IPA's Mach-O must be
decrypted (`cryptid 0`). So is kernel code-signing on later iOS majors — that
needs its own per-major-version work and this dylib does not touch the kernel.

## How it binds (nothing at a fixed offset)

| Gate | Where | How it's found | What the hook does |
|---|---|---|---|
| Signature validation | `installd` → `libmis` `MISValidateSignature` | **symbol**, dyld `__interpose` | returns success |
| Signature + signer info | `installd` → `libmis` `MISValidateSignatureAndCopyInfo` | **symbol**, dyld `__interpose` | returns success **and** fills the out `info` dict with `kMISValidationInfoValidatedByProfile = true` and `kMISValidationInfoSignerCertificate = <cert>` — the two fields `installd`'s `verify_signer_identity` reads back |
| App launch trust | `SpringBoard` `-[SBApplication applicationSignatureState]` | **ObjC selector**, `method_setImplementation` at load | returns `2` (trusted) |

Why the info dict is populated and not left empty: returning `0` with an empty
dict is *not* enough — `installd` then errors on the missing
`SignerCertificate`. This is the same gate the iPod recipe clears by NOPing
`installd`'s profile-validation branch (`installd 0x605C` on 7E18); here it's
cleared by giving `installd` the result it wants, which needs no `installd`
offset at all.

The MIS keys (`kMISValidationInfo*`) are `libmis` exports that `installd`
already imports, so they bind by name. `SBApplication` is absent in `installd`,
so the SpringBoard constructor is a no-op there; the two processes share one
binary.

### The one embedded fact

`appsync_cert.h` is a self-signed DER certificate. `installd` only reads its
subject summary (for logging/identity), so **any** valid cert satisfies it —
it is not tied to a firmware, a device, or an app. Regenerate with
`build.sh --regen-cert`.

## Build

```
contrib/appsync/build.sh            # -> build/appsync/libappsync.dylib (fat armv6+armv7, ldid -S)
```

armv6 slice ← 3.1.3 SDK (`~/Developer/ipod2g-re/OldSDK/iPhoneOS3.1.3.sdk`),
armv7 slice ← 3.2 SDK. Both go through the `contrib/armv6-toolchain` pipeline
(`-marm`, ld-as-armv7, `mkold.py` strips modern load commands) and link with
`-undefined dynamic_lookup` so the MIS/CF/Security/objc symbols resolve in the
host process, not against the (old, "built for unknown") SDK frameworks.

## Install it

### iPad (3.2.2)
```
imgtools/ipad1_rootfs.py build --base pristine --appsync   # + installs libappsync.dylib, injects both jobs
imgtools/ipad1_rootfs.py bake  <build dir>                  # it_pbd, as golden-pristine
imgtools/ipad1_nand.py   build ... --out .../golden-appsync.new
chmod -R a-w golden-appsync.new && mv golden-appsync.new golden-appsync
```
`--appsync` copies the dylib to `/usr/lib/libappsync.dylib` (root-owned) and
adds it to `DYLD_INSERT_LIBRARIES` in `com.apple.mobile.installd.plist` and
`com.apple.SpringBoard.plist`. The dylib is `ldid -S` signed, so the store must
boot with the AMFI boot-args (`amfi_allow_any_signature=1 cs_enforcement_disable=1`,
the default in `7B500/k48-kboot.bin`).

### iPod (3.1.3)
```
qemu-ios-files/apps/patch-appsync.sh --dylib   # baker option: install libappsync.dylib instead of the 4 byte patches
```
The old four-patch recipe is kept alongside (`--dylib` opt-in) until the dylib
route is the default.

## Verified

- iPad 1 / 3.2.2 (7B500): install + launch of a decrypted 3.x IPA — see
  `docs/ipad1/screens/`. <!-- filled in after the proof boot -->
- iPod touch 2G / 3.1.3 (7E18): install + launch + full `tests/ipod/regress.py`
  default tier. <!-- filled in after the iPod run -->

## Out of scope

- Encrypted (FairPlay) apps.
- Kernel code-signing enforcement on iOS majors newer than these; per-major work.
- The dylib does not re-sign on-disk binaries: any Mach-O *edited* on disk still
  needs `ldid -S` (page hashes), as documented in `imgtools/README-appsync.md`.
