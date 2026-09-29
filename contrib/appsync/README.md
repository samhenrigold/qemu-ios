# AppSync: IPSW-agnostic decrypted-app install/launch

Two pieces, both located by **symbol** — never by a hand-found per-firmware byte
offset — that let a **decrypted** IPA install and launch on the emulated legacy
devices (iPod touch 2G / 3.1.3, iPad 1 / 3.2.2):

1. **A shared-cache patch** (`imgtools/appsync_cachepatch.py`): forces
   `libmis`'s `MISValidateSignature` to return success inside
   `dyld_shared_cache_armv7/armv6`, located by parsing each cached image's own
   `LC_SYMTAB` (no offset table, no SDK) and byte-checking the Thumb prologue
   before patching. This is the iPod's "patch 3" done by symbol.
2. **A fat armv6+armv7 dylib** (`contrib/appsync`), injected into **`installd`
   only** via `DYLD_INSERT_LIBRARIES`.

It replaces the iPod's four fixed-offset patches (`imgtools/README-appsync.md`)
with symbol-bound hooks that carry across firmware versions unchanged.

Encrypted FairPlay bundles are out of scope; the IPA's Mach-O must be decrypted
(`cryptid 0`). So is kernel code-signing on later iOS majors — that needs its own
per-major-version work and none of this touches the kernel.

## How it binds (nothing at a fixed offset)

| Gate | Where | How it's found | What it does |
|---|---|---|---|
| Global signature check (amfid, launch) | `libmis` `MISValidateSignature` in the shared cache | **symbol** (image `LC_SYMTAB`), prologue patched to `movs r0,#0; bx lr` | returns success everywhere — so amfid approves the ldid-signed dylib and decrypted app code, and SpringBoard's launch gate passes |
| installd signature + signer info | `installd` → `MISValidateSignatureAndCopyInfo` | **symbol**, dyld `__interpose` | returns success and fills the out `info` dict with `ValidatedByProfile = true` + a `SignerCertificate` CFData |
| installd signer-cert acceptance | `installd` → `SecCertificateCreateWithData`, `SecCertificateCopySubjectSummary` | **symbol**, dyld `__interpose`, gated to installd via `getprogname` | makes `verify_signer_identity`'s cert path succeed; other processes call through to the real functions via `dlsym(RTLD_NEXT)` |

Why the Sec interposes: `installd`'s `verify_signer_identity` calls
`SecCertificateCreateWithData` on the signer cert, and on a freshly built store
(no working keychain / Security trust store) that returns `NULL` even for a valid
Apple DER — `ApplicationVerificationFailed`. Interposing the two Sec calls that
`installd` imports sidesteps the Security stack entirely, in installd only.

**Only `installd` is injected.** Injecting the dylib into SpringBoard breaks its
boot (SpringBoard imports the same `SecCertificate*` symbols; the interposes wedge
its startup). SpringBoard needs no injection: the shared-cache
`MISValidateSignature` patch already makes its launch-trust gate pass, so a
tapped icon launches. The `-[SBApplication applicationSignatureState]` swizzle in
the dylib is therefore inert here (kept for reference / other firmwares) — the
dylib is not loaded into SpringBoard.

### The one embedded fact

`appsync_cert.h` is a real Apple DER cert (`iPhoneActivation`, from the 3.2.2
rootfs) used only as the info-dict `SignerCertificate` value; with the Sec
interposes its content no longer matters. Regenerate with `build.sh
--regen-cert`.

## iPhone OS 2.x and 3.0

The armv6 slice now uses `LEGACY_LINK=1`: classic relocations, no `LC_DYLD_INFO_ONLY`,
and r9 reserved for the 2.x thread pointer. The same slice remains usable by 3.1+.
The armv7 build is unchanged.

Early firmware's standalone libmis must stay stock. Modifying/re-signing it prevented
2.1.1 startup; restoring just that file restored boot. The emulator's standard
`amfi_allow_any_signature=1 cs_enforcement_disable=1` boot arguments already allow
ad-hoc app execution, so process-local installation interposition is sufficient.

3.0 has an installd job and uses the existing injection path. On 2.x, Lockbot launches
`mobile_installation_proxy` and only forwards ProgramArguments, ignoring EnvironmentVariables.
`appsync-launch` sets DYLD_INSERT_LIBRARIES and execs the original service with its arguments.
It uses an explicit ARM entry trampoline: old dyld enters with argc/argv on the stack.
Both new artifacts are exported automatically; no user path or setting is involved.

Verified 2026-09-29 on fresh FirmwareKit 5F138 and 7A341 images: an ad-hoc ARMv6 UIKit
smoke IPA installs and visibly launches. The same IPA on the AppSync-off 5F138 control
fails with ApplicationVerificationFailed. No shared-cache, SpringBoard, or standalone
libmis patch is needed for these early builds. Other 2.x point releases use the same
service selection but still need individual app compatibility testing.

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

### iPad (3.2.2) — golden-appsync, sealed recipe
```
contrib/appsync/build.sh                                    # libappsync.dylib
contrib/ipad1-guest/build.sh                                # it_pbd, it_seal, ...
imgtools/ipad1_rootfs.py build --base pristine --appsync --out <W>
imgtools/ipad1_rootfs.py bake  <W>/pristine --seal
imgtools/ipad1_nand.py   build --mbr .../rdisk0-head4M.bin --system <W>/pristine/system.img \
                               --data <W>/pristine/data.img --out .../golden-appsync.new
imgtools/ipad1_seal.py   .../golden-appsync.new            # one clean-halt boot, seals the FTL
chmod -R a-w golden-appsync.new && mv golden-appsync.new golden-appsync
```
`--appsync` patches the shared cache (`appsync_cachepatch.patch_cache`), copies
the dylib to `/usr/lib/libappsync.dylib` (root-owned), and adds it to
`DYLD_INSERT_LIBRARIES` in **`com.apple.mobile.installd.plist` only**. The dylib
is `ldid -S` signed, so the store boots with the AMFI boot-args
(`amfi_allow_any_signature=1 cs_enforcement_disable=1`, the default in
`7B500/k48-kboot.bin`). Never boot golden in place; clone (`cp -cR`) or use
`nand-overlay`.

### iPod (3.1.3)
On a copy of an image made with the old four-patch recipe (e.g. nand-agent-v4),
with the stock 7E18 rootfs (`ios3/rootfs313.dmg`) mounted at `$STOCK`:
```
STOCK_ROOT=$STOCK imgtools/editimg.py --nand <copy> --blocks 1835008 \
    --script contrib/appsync/patch-appsync-dylib.sh
imgtools/setowner.py --nand <copy> /usr/lib/libappsync.dylib:0:0:644 \
    /usr/libexec/installd:0:0:755 \
    /System/Library/CoreServices/SpringBoard.app/SpringBoard:0:0:755
```
`STOCK_ROOT` puts back the Apple-signed installd and SpringBoard, so no byte
patch remains besides the symbol-found cache patch.

## Verified

- **iPad 1 / 3.2.2 (7B500): install + launch, 2026-09-27.** `ideviceinstaller
  install` of a decrypted Cube Runner IPA over the usbmuxd-qemu bridge succeeds;
  `ideviceinstaller list` shows `com.andyqua.CubeRunner`; the icon appears on
  SpringBoard and tapping it launches the app. See
  `docs/ipad1/screens/appsync-cuberunner-icon.png` and
  `appsync-cuberunner-running.png` (the app runs in the iPhone-compat window; its
  3D content renders black only because GL compositing is off on this store —
  unrelated to AppSync).
- iPod touch 2G / 3.1.3 (7E18), 2026-09-27: `qemu-ios-files/nand-current.new`
  (nand-agent-v4 + stock installd/SpringBoard + this dylib + current GLES shim)
  passes all 8 default-tier `tests/ipod/regress.py` checks, and a Harness copy
  with its code signature stripped (`ldid -r`) installs and launches.

## Out of scope

- Encrypted (FairPlay) apps.
- Kernel code-signing enforcement on iOS majors newer than these; per-major work.
- The dylib does not re-sign on-disk binaries: any Mach-O *edited* on disk still
  needs `ldid -S` (page hashes), as documented in `imgtools/README-appsync.md`.
