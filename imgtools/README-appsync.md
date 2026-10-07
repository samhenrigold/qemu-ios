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

The installation-service dylib binds by symbol, preserves real signing metadata,
and supplies the legacy fallback only when no info is returned. Neither the
shared cache nor stock installd/SpringBoard is patched for AppSync. The emulator's
AMFI boot arguments permit ad-hoc execution; the helper handles installation.
See [the current implementation](../contrib/appsync/README.md).

The four fixed 7E18 offsets in old nand-agent-v4 images and the later symbol-found
shared-cache patch are historical approaches. Rebuild from an IPSW to remove
those old edits; no generic unpatch is applied to existing images.

## Signing and baked tools

The guest helpers are ldid-signed at build time. Preparation retains Apple's
standalone installd and SpringBoard binaries, signatures and entitlements.
Historical byte-patching experiments required explicit entitlement preservation;
those scripts are not part of this preparation path.

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
