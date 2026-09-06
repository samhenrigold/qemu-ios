# Decrypted app installation on 3.1.3 / 7E18

The current target is iPod touch 2G running **3.1.3 / 7E18**. Light Touch's
prepared image (`nand-agent-v4`) already contains the install/launch changes
and guest tools. Use the app's Install App command or the regression harness;
do not run the old two-patch 2.1.1 recipe over this image.

An IPA must contain a decrypted ARMv6-compatible executable and compatible
framework imports. SDK metadata alone cannot establish compatibility.
GLES 1.1 apps are supported by the emulator's MBX bridge; they are not excluded
as the original 2.1.1 notes suggested. Encrypted FairPlay bundles remain outside
this recipe. See [the app ledger](../docs/app-ledger.md) for measured coverage.

## Four patch sites in the prepared image

These are **file offsets for the identified 7E18 binaries**, not addresses to
apply blindly to other releases. The existing preparation scripts verify original
or already-patched bytes before editing.

| Site | Original → replacement | Purpose |
| --- | --- | --- |
| `usr/libexec/installd`, `0x9F34` | `005050e2` → `0050b0e3` | Treat signature verification as successful. |
| `usr/libexec/installd`, `0x605C` | `0500000a` → `0000a0e1` | Remove the conditional profile-validation branch. |
| `dyld_shared_cache_armv6`, `0x1750EF8` | `80b500af` → `00207047` | Return success from `MISValidateSignature`. |
| `SpringBoard`, `0x17D1C` | `f0b503af` → `02207047` | Return trusted state from `applicationSignatureState`. |

On 3.1.3, libmis exists inside the shared cache; there is no standalone
`/usr/lib/libmis.dylib` to patch. The SpringBoard site is Thumb code, unlike the
2.1.1 ARM method. The first three sites were independently required in recorded
experiments. The fourth is part of the working recipe; its independent necessity
on 3.1.3 was not established by the old inconclusive A/B run.

The local firmware workspace holds the image preparation scripts:
`qemu-ios-files/ssh/patch-cache.sh` handles the cache and
`qemu-ios-files/apps/patch-appsync.sh` handles installd/SpringBoard. Inspect those
scripts and their version checks before preparing another **copy**. Never edit a
running image, the default base, or reuse an overlay with a different base.

## Signing and baked tools

Re-sign changed standalone Mach-O binaries so their CodeDirectory page hashes
match. **Preserve SpringBoard's stock entitlements** with the explicit entitlement
file used by `patch-appsync.sh`. Bare `ldid -S` removes them; this previously broke
keychain and iTunes messaging. The preparation script checks that all eight stock
entitlement keys survive. Stock installd has no entitlements. The shared-cache
patch does not use standalone Mach-O re-signing.

`imgtools/bake-guest-tools.sh` installs the agent, typing bridge, GLES engine and
launch helpers. Follow its documented ownership repair: files created in a
`noowners` host mount do not automatically gain the guest's required root ownership.
The [agent protocol](../contrib/it-agent/README.md) documents transport and checks.
Prepared-image acceptance must include an actual install and foreground launch,
not merely copying a bundle or observing an installer success message.

## Historical tools

- `patch_codesign_gate.py` and `patch_libmis.py` are **2.1.1 / 5F138 only**.
- `patch_springboard.py` recognizes both documented 2.1.1 and 3.1.3 prologues;
  it only edits bytes and does not replace the signing/entitlement steps above.
- `free_disk_space.py` is the old 500 MB image's destructive English-only pruning
  recipe, not a preparation step for the current larger image.
- `cdverify.py` checks standalone CodeDirectory page hashes.

The historical Obama '08 screenshot establishes that old experiment's launch,
not a current compatibility verdict for every release or app.
