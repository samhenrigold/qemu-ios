# Decrypted app installation on 3.1.3 / 7E18

The current target is iPod touch 2G running **3.1.3 / 7E18**. Light Touch's
device (built from the stock IPSW by `ipod2g_device.py`) already contains the
install/launch changes and guest tools. Use the app's Install App command or the regression harness;
do not run the old two-patch 2.1.1 recipe over this image.

An IPA must contain a decrypted ARMv6-compatible executable and compatible
framework imports. SDK metadata alone cannot establish compatibility.
GLES 1.1 apps are supported by the emulator's MBX bridge; they are not excluded
as the original 2.1.1 notes suggested. Encrypted FairPlay bundles remain outside
this recipe. See [the app ledger](../docs/app-ledger.md) for measured coverage.

## How the gates are opened now

Nothing is patched at a fixed offset. `imgtools/appsync_cachepatch.py` finds
`MISValidateSignature` in the shared cache by symbol and makes it return success
(amfid's half), and the `contrib/appsync` dylib, injected into installd and
SpringBoard, answers the install and launch gates. The four fixed 7E18 offsets
the old `nand-agent-v4` image carried (installd twice, the cache, SpringBoard)
and their byte patchers are retired; they are in git history
(`docs/archive/backport-from-ipad1.md`).

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

## Other tools

- `cdverify.py` checks standalone CodeDirectory page hashes.

The historical Obama '08 screenshot establishes that old experiment's launch,
not a current compatibility verdict for every release or app.
