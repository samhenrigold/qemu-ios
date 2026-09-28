# nand-current.new vs nand-current (iPod touch 2G, iOS 3.1.3 / 7E18)

Verified 2026-09-27 on `ipad1-periph` (ipad1 merged in, `build/qemu-system-arm` rebuilt).

- OLD = `qemu-ios-files/nand-current` -> `nand-agent-v4` (byte-patched installd 0x9F34/0x605C and SpringBoard 0x17D1C).
- NEW = `qemu-ios-files/nand-current.new` (stock installd/SpringBoard, `libappsync.dylib` in installd, new GLES shim).

Both images stayed read-only. Every boot used a throwaway overlay.

## 1. File-level diff

Method: `imgtools/dumpvol.py --blocks 1835008` for each image, attached and mounted read-only, then
`tests/ipod/nand_manifest.py`. That script takes owner, mode and dates from the HFS+ catalog, because the host mount is
`noowners`, and takes SHA-256 from the mount. The iPod image is one HFS+ volume (no separate `/private/var`
partition, see `imgtools/build_nand.py`), so "data volume" content is `/private/var` inside it and is covered.
Both volumes pass `fsck_hfs -n`. Catalog records: OLD 16917, NEW 16918.

| Path | Change |
|---|---|
| `/usr/libexec/installd` | content (572880 -> 568160), **owner 99:99 -> 0:0**, mode 100755 both. SHA-256 matches `rootfs313.dmg` byte for byte. |
| `/System/Library/CoreServices/SpringBoard.app/SpringBoard` | content (1205472 -> 1195312), **owner 99:99 -> 0:0**, mode 100755 both. SHA-256 matches `rootfs313.dmg`. |
| `/System/Library/LaunchDaemons/com.apple.installd.plist` | 202 -> 284 bytes, 0:0 100644 both. OLD equals stock. NEW's only change is `EnvironmentVariables = { DYLD_INSERT_LIBRARIES = /usr/lib/libappsync.dylib }` (XML diff of the binary plists). |
| `/usr/lib/libappsync.dylib` | added, 0:0 100644, 134336 bytes. SHA-256 equals `build/appsync/libappsync.dylib`. |
| `.../OpenGLES.framework/MBXGLEngine.bundle/MBXGLEngine` | 116820 -> 135092, 0:0 100755. SHA-256 equals `contrib/it-gles/MBXGLEngine`. |
| `/usr/local/bin/sblaunch` | **timestamp only**: content identical (51360, same SHA-256 as `contrib/it-gles/sblaunch`). Only the mod dates changed. |
| `/usr/lib` (dir) | timestamp only (a new entry was added) |
| `/` (volume root) | timestamp only |

Nothing else differs: no path added or removed, no mode, owner, size or hash change anywhere else. That includes
all of `/private/var` (installation caches, Lockdown, preferences), the dyld shared cache (its MISValidateSignature
patch is byte-identical in both) and the extended-attributes B-tree.

Page-level cross-check: every page file was hashed in both images, including the 64-byte spare area. No page was
removed, none sits outside the volume, and no page differs only in its spare area. Changed pages are 11 catalog
nodes, 3 bitmap blocks and the volume header. The 471 added pages all belong to the files above. The 42 added
pages in free space are leftover writes from editimg that the bitmap marks free, so they are harmless.

Unexpected differences, all harmless: the OLD installd and SpringBoard were owned by uid/gid 99, and NEW corrects
them to 0:0. sblaunch is re-dated but not changed.

OLD vs stock, to confirm what NEW removes: SpringBoard differs only at 0x17D1C, which is
`movs r0,#2; bx lr` at the entry of `-[SBApplication applicationSignatureState]`, plus the code-signature load
command. installd differs only at 0x605C and 0x9F36, plus the load-command sizes.

Code signatures (`imgtools/cdverify.py`) on NEW:
- installd: 138/138 pages match.
- SpringBoard: 291/291.
- sblaunch: 13/13.
- libappsync armv6 and armv7 slices: 13/13 each.
- The MBXGLEngine shim has no `LC_CODE_SIGNATURE`. The same is true on OLD, so this is not a change.

## 2. App launch pass (OLD vs NEW)

Tool: `tests/ipod/app_ab.py`. Each (image, IPA) pair is one `regress.py --checks boot,appinstall,applaunch
--launch-stages` run on its own fresh overlay:
- install over usbmuxd with `ideviceinstaller`
- launch through SpringBoard (`SBSLaunchApplicationWithIdentifier`)
- screenshots at 5 s and 20 s
- at 30 s, require that this exact app is frontmost and the screen is lit
- pull CrashReporter, installd's `com.apple.mobile.installation.plist` and `com.apple.springboard.plist`

Runs execute in parallel with disjoint port ranges.

App set: every decrypted IPA in `~/Downloads/ios3` with an armv6 slice and MinimumOSVersion <= 3.1.3, one per
bundle ID, plus Harness. That is the iPad's 49-app set minus the armv7-only builds. `qemu-ios-files/apps` and
`qemu-ios-files/ios3` contain no IPAs.

| App | OLD | NEW |
|---|---|---|
| com.qemuios.harness | PASS | PASS |
| com.imangi.templerun | PASS | PASS |
| ssacross.com.simonandschuster (365XWords) | PASS | PASS |
| com.clickgamer.AngryBirds | PASS | PASS |
| com.classicsapp.classics | PASS | PASS (first try: USB never enumerated, see §3) |
| com.condenet.Epicurious | PASS | PASS |
| com.tapulous.katyperryfree | FAIL: SBS launch refused | FAIL: SBS launch refused |
| com.tapulous.taptaprevengeIII | PASS | PASS |
| com.yelp.yelpiphone | PASS | PASS |
| com.andyqua.CubeRunner | FAIL: not frontmost, no crash log | FAIL: same |
| com.tapulous.ttr2 (Dance) | PASS | PASS |
| com.yourcompany.DoodleJump (1.1_os20) | PASS | PASS |
| com.apple.ist.ecapture | PASS | PASS |
| com.facebook.Facebook | PASS | PASS |
| com.apple.FieldScout | PASS | PASS |
| com.burbn.instagram | PASS | PASS |
| com.atebits.Tweetie2 | PASS | PASS |
| com.jwegventures.exitstrategy | PASS | PASS |
| com.freeverse.Postman | PASS | PASS |
| com.tapulous.taptapboost | FAIL: SIGILL crash (EXC_BAD_INSTRUCTION 0xe958f0ee) | FAIL: same |
| com.tapulous.gaga | PASS | PASS |
| com.bluetechnologysolutions.iTransitBuddyMetroNorth | FAIL: not frontmost, no crash log | FAIL: same |
| com.tapulous.ladygaga3 | PASS | PASS |
| com.apple.Magneto | FAIL: dyld `Symbol not found: _kCLErrorHeadingFailure` (API newer than 3.1.3) | FAIL: same |
| com.sparkfishcreative.masstransit | PASS | PASS |
| com.pandora | PASS | PASS |
| se.codify.labyrinth | PASS | PASS |
| com.shazam.Shazam | PASS | PASS |
| com.starbucks.mystarbucks | PASS | PASS |
| com.ooi.supermonkeyball | FAIL: not frontmost, no crash log | FAIL: same |
| com.tapulous.taptaprevenge4 | PASS | PASS |
| com.tapulous.coldplay | PASS | PASS |
| com.tapulous.taptaprevengeII | PASS | PASS |
| com.tapulous.TapTap | PASS | PASS |

**28 pass and 6 fail on both images. There are 0 regressions and 0 improvements.**

The four failures that leave no crash report (Cube Runner, Super Monkey Ball, iTransitBuddy, Katy Perry) were
re-run alone, at low host load, and failed the same way on both images again. They are pre-existing issues and are
not caused by the NEW image. Their root causes were not investigated here.

Screenshots were compared per pixel between OLD and NEW at 5 s, 20 s and 30 s:
- At 20 s and 30 s every app differs by at most 1.7% of pixels. Angry Birds and Doodle Jump differ slightly because
  they animate. Harness's 1.2% is its container UUID text.
- At 5 s, only ecapture and Yelp differ noticeably, because splash-screen timing varies.
- Crash-report counts are identical per app.

**libappsync is loaded in installd on NEW.** Every app installed on NEW is registered by the stock installd with
`SignerIdentity = "AppSync"` and `ProfileValidated = true`:
- `"AppSync"` is the return value of libappsync's interposed `SecCertificateCopySubjectSummary`. The standalone
  string exists nowhere else on the volume; the only other hits are the substring in `_CFPreferencesAppSynchronize`.
- `ProfileValidated` comes from the interposed `MISValidateSignatureAndCopyInfo` (`ValidatedByProfile = true`).

On OLD the same IPAs register with `SignerIdentity = nil` (Harness, ldid-signed) or
`"Apple iPhone OS Application Signing"` (Angry Birds).

**Why stock SpringBoard's launch gate passes on NEW.** 3.1.3's SpringBoard does not import
`MISValidateSignature`, so the shared-cache patch plays no part here. Disassembly of stock
`-[SBApplication applicationSignatureState]` (0x18d1c) shows this logic:
1. It returns 2 (trusted) if `_isSystemApplication` is set or `_signerIdentity` is nil.
2. Otherwise it checks the provisioning-profile expiry and calls `_signatureNeedsExplicitUserTrust` (0x1cd38), which
   returns NO in three cases:
   - `_provisioningProfileValidated` is false;
   - the signer identity is already in `SBTrustedCodeSigningIdentities`;
   - no installed provisioning profile matches the signer identity (`_doesProfileMatchSignerIdentity:`). In that
     case it also calls `markApplicationIdentityAsTrusted`.
   A NO here makes the state 2.

On NEW the identity is "AppSync" and ProfileValidated is YES. No provisioning profile on the device matches
"AppSync", so the gate returns 2 and records the identity. Observed: after one launch on NEW,
`com.apple.springboard.plist` has `SBTrustedCodeSigningIdentities = ["AppSync"]`. OLD has no such key, because its
patched gate returns 2 before reaching this code.

Consequence: the gate stays open only while no installed profile matches the signer "AppSync". An expired matching
profile would give state 0, and a matching profile that doesn't include this device would give state 1. Neither
case can arise without deliberately installing such a profile.

## 3. Flakiness: `regress.py` default tier (8 checks), 3 runs per image

| Check | OLD #1 | OLD #2 | OLD #3 | NEW #1 | NEW #2 | NEW #3 |
|---|---|---|---|---|---|---|
| boot | PASS | PASS | PASS | PASS | PASS | PASS |
| fsck | PASS | PASS | PASS | PASS | PASS | PASS |
| persist | PASS | PASS | PASS | PASS | PASS | PASS |
| appinstall | PASS | PASS | PASS | PASS | PASS | PASS |
| applaunch | PASS | PASS | PASS | PASS | PASS | PASS |
| gles | **FAIL** | **FAIL** | **FAIL** | PASS | PASS | PASS |
| agent | PASS | PASS | PASS | PASS | PASS | PASS |
| audio | PASS | PASS | PASS | PASS | PASS | PASS |

OLD's gles failure reproduces on every run:
`the app called 2 unimplemented entry point(s): 305 (glTexParameteriv), 817 (glDrawTexfOES)`. OLD ships a
guest GLES shim that predates 551a7eb002 and 567321152a, the commits that filled those slots. It is out of step with
the current emulator, and NEW's new shim is what fixes it.

In the app pass, one of 41 NEW boots failed: Classics, in the first batch of 20 QEMUs running at once. usbmuxd's
USB enumeration never got past `SETUP never accepted (NAK)` in 78 attempts. That run was discarded, and on re-run
it passed at lower host load. It was not seen in any other boot (about 40 OLD, 40 NEW). This is host-load/USB
enumeration flakiness and is not tied to the image.
