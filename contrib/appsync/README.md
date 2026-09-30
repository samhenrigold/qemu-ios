# AppSync: automatic decrypted-app installation

One fat armv6+armv7 dylib, injected only into the **installation service**, lets
compatible decrypted IPAs install on emulated iOS 2.x-5.x. Symbols are resolved
by name; no firmware-specific offsets, Substrate installation, user-selected
hook, or third-party package installation is needed.

- 3.0+ uses installd's launchd job.
- 2.x uses Lockbot's mobile_installation_proxy service. Lockbot ignores
  EnvironmentVariables, so `appsync-launch` supplies DYLD_INSERT_LIBRARIES and
  execs the original service with its original arguments.
- The system libmis and shared cache remain stock. The standard emulator kernel
  boot arguments (`amfi_allow_any_signature=1 cs_enforcement_disable=1`) permit
  ad-hoc execution. AppSync supplies installation signing information locally.
- SpringBoard is not injected or swizzled. Stock installd and SpringBoard are
  retained; third-party replacement Apple daemons are not distributed.

The helpers remain automatic preparation artifacts. Rebuild devices to get this
behavior; an existing image may still contain its earlier shared-cache patch.
The historical `imgtools/appsync_cachepatch.py` inspector remains available for
research, but neither preparation pipeline invokes it for AppSync.

## Hook behavior

| Hook | Behavior |
| --- | --- |
| MISValidateSignature | In an installation service, success (the old API has no metadata); elsewhere the original result |
| MISValidateSignatureAndCopyInfo | Calls the original and preserves its complete non-null result dictionary; when no dictionary is returned, supplies the legacy ValidatedByProfile and SignerCertificate fields |
| SecCertificateCreateWithData | Calls the original; only if it rejects our exact embedded DER in an installation service, retains that CFData as a certificate stand-in |
| SecCertificateCopySubjectSummary | Synthesizes a summary only for that stand-in; otherwise calls the original |

The CoreFoundation dictionary callback exports are **structs**. Their addresses
must be passed to dictionary creation so the keys and signer data are retained.
The previous pointer declarations accidentally passed null callbacks and left
SignerCertificate dangling after its local CFRelease.

The original-first metadata and selective certificate fallback follow AppSync
Unified's behavior; see [prior-art.md](prior-art.md). Its richer metadata
synthesis is gated for iOS 8+, outside this project's current supported range.
The embedded certificate in appsync_cert.h is used only for the legacy fallback;
`build.sh --regen-cert` regenerates it. The fallback compares its bytes exactly.

## Build and check

```
bash contrib/appsync/build.sh
bash contrib/appsync/test-hooks.sh
```

The armv6 slice uses the 3.1.3 SDK, classic relocations (`LEGACY_LINK=1`), no
LC_DYLD_INFO_ONLY, and r9 reserved for the 2.x thread pointer. The armv7 slice uses
the 3.2 SDK. Both link with undefined dynamic lookup for host CF/Security/MIS
symbols and are ldid-signed. The explicit launcher ARM entry trampoline accepts
old dyld's argc/argv stack convention.

The export script builds/stages both helpers. FirmwareKit and the Python device
bakers insert them into the appropriate service. For a historical iPod image,
`patch-appsync-dylib.sh` can restore standalone stock installd/SpringBoard from
STOCK_ROOT, but does not undo an already patched shared cache.

The native test uses real CoreFoundation objects under ASan/UBSan to check
original metadata preservation, Copy ownership, exact certificate scope, and
installation-process scope. Guest acceptance requires actual installation and
foreground launch, plus an AppSync-disabled control where practical. See
LightTouchMac's docs/appsync-cleanup.md for this change's acceptance results.

## Limits

The IPA executable must be decrypted (cryptid 0), match the device architecture,
and use frameworks that its firmware actually provides. FairPlay decryption is
outside this component. Kernel code-signing policy belongs to the emulator's
boot recipe; the dylib does not modify the kernel or re-sign system binaries.
