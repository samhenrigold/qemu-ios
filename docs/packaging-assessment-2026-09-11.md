# Light Touch packaging and firmware assessment

Historical assessment, written before implementation. The first cleanup phase
is now described in the [product build instructions](../../LightTouchMac/README.md),
[repository layout](../../LightTouchMac/docs/repository-layout.md)
and [macOS storage audit](../../LightTouchMac/docs/storage-layout.md).
Firmware remains bundled; user-supplied firmware work is deferred.

Assessment date: September 11, 2026. This is a proposed direction based on read-only inspection of the six supplied folders and primary-source legal research. No existing source, firmware, releases, build scripts, or Git history were changed. Existing documentation was treated as evidence about the project, not as instructions to execute its procedures. This document is the sole new artifact.

**Recommendation: make LightTouchMac the product repository and sole release entry point, preserve QEMU and usbmuxd as upstream forks, and develop a firmware-free distribution with local firmware preparation.** User-supplied IPSW is a worthwhile goal, but the present scripts cannot reproduce the complete device from an IPSW alone. It reduces firmware redistribution exposure; it does not settle licensing, circumvention, or source-provenance questions.

**The current folder layout mixes six different kinds of material.**

| Current folder | Observed role | Recommended destination |
|---|---|---|
| `LightTouchMac` | Native Mac app, dependency build, packaging and app tests; about 30 MB | Product repository: app, release orchestration, dependency recipes, lock manifest, release documentation |
| `qemu-ios` | QEMU fork, device models, guest helpers, image utilities, old frontends and build outputs; about 19 GB | Preserve fork/history; retain tightly coupled emulator and guest tools here initially; move outputs out of source directories |
| `usbmuxd-qemu` | Small wrapper around a nested usbmuxd fork, plus transport fixture and docs; about 17 MB | Pin actual usbmuxd fork directly; preserve useful wrapper fixture/docs before retiring the wrapper |
| `qemu-ios-deps12` | Unversioned installed dependency prefix; about 139 MB | Move build recipes into product Git; regenerate prefix as a cache/output |
| `qemu-ios-files` | Boot assets, many NAND generations, overlays, apps, backups and loose scripts; about 19 GB | Private input/image/state/evidence directories outside product Git; selectively promote authored utility source |
| `ipod2g-re` | SDKs, extracted firmware, disassembly, research notes, Apple source tree; about 7 GB | Separate research archive with provenance records; product builds should use only explicitly declared, reviewed inputs |

Sizes are filesystem usage, not release download sizes. The last three folders are not Git repositories. Turning every folder into a repository would preserve the underlying ambiguity about what is source, input, cache, and state.

**Use three source repositories and one release manifest.** Keep LightTouchMac, qemu-ios, and the actual usbmuxd fork. The product manifest should select full commit IDs for both forks and the committed Swift package resolution. It should also record dependency archive URLs and hashes, local patch hashes, toolchain/SDK versions, architecture, deployment target, and the guest-tool/firmware-recipe versions.

Source staging can use ignored checkouts selected by that manifest. Avoid adding a fourth umbrella repository or another nested wrapper solely to coordinate versions. Keep QEMU's upstream history and remotes; defer cosmetic moves of guest helpers until ownership and interface boundaries are established. Consolidate duplicate `origin`/`fork` aliases only after preserving any distinct configuration. Existing historical tags/branches should remain resolvable.

Local Git baseline, without fetching:

| Component | Observed revision and state |
|---|---|
| LightTouchMac | `4d77bb5650bd502549eac31636343a3e3033be48`; clean |
| qemu-ios | `9a31bb069ad3811f3eabea555c78f9e2d5d9ee59`; four modified media/photo source/test files |
| usbmuxd-qemu wrapper | `660e46876039c48341fc23477adf2bf368e4c3be` |
| Nested usbmuxd | `a0859eb8a2e724b5c0ff59b81c0539dcb003c05e`; modified `src/main.c` and tracked `.DS_Store`; three commits ahead of cached fork branch |

The nested usbmuxd directory has its own Git directory, while the parent reports its gitlink as uninitialized. A fresh clone has not been shown to reproduce this working directory. Preserve local changes and unpublished commits before migration. These observed revisions are an inventory, not a tested release lock.

**Keep the useful build machinery; replace its hidden inputs.** The [native dependency builder](../../LightTouchMac/scripts/build-package-native.sh) already verifies nine source archive hashes. The existing Mach-O closure validator, signing sequence, and device-state code are also useful foundations.

The concrete problems are:

- The builder still consumes an existing dependency prefix and copies prebuilt clients and a potentially dirty usbmuxd checkout. See [build-package-native.sh](../../LightTouchMac/scripts/build-package-native.sh). The prefix's own recipe depends on a personal path and an old job's scratch directory: [build-deps12.sh](../../qemu-ios-deps12/build-deps12.sh).
- The native packager gets `ipod-helper` from a previously built, different application. Compile that source as a product build target: [package.sh](../../LightTouchMac/scripts/package.sh).
- Guest tools are copied from existing output files rather than rebuilt by a complete release graph. The ARMv6 toolchain requires an external legacy SDK: [armv6.sh](../contrib/armv6-toolchain/armv6.sh). Several helper binaries are also tracked in Git. Record their provenance and replace reliance on them with source builds before removing them.
- QEMU's dylib is produced by rewriting an executable link command. Give the dylib a real QEMU build target, then export its library, public headers and ABI version as an artifact: [make-dylib-macos.sh](../contrib/macos-app/make-dylib-macos.sh).
- Xcode contains developer-directory paths for QEMU integration: [project.pbxproj](../../LightTouchMac/LightTouchMac.xcodeproj/project.pbxproj). Generate build settings from the selected artifact locations.
- Firmware selection depends on which named NAND directories happen to exist: [package.sh](../../LightTouchMac/scripts/package.sh). A release must choose inputs explicitly.
- `LTM_ASSETS=none` skips copying firmware, but does not establish consumer setup, and does not clear a previously populated device directory in a reused app bundle. Always assemble a fresh release bundle and verify its complete inventory.

The intended build should have one entry point for dependencies, QEMU, host tools, guest tools, the Xcode app, packaging and verification. Scripts can remain small implementation steps under that entry point. Signing/notarization follows assembly and validation, using the same declared inputs. Treat the currently supported native product as Apple Silicon/macOS 14; the older macOS 12 launcher is a separate historical configuration, not evidence that the current app supports 12.

Release mode should resolve tools and dynamically loaded libraries only through declared bundle or system locations. Disable developer-checkout/Homebrew fallbacks there; keep them as explicit development settings. Static library-closure checks cannot discover every library opened dynamically or tool launched by a script, so clean-machine acceptance remains necessary.

Suggested product-owned layout, with names illustrative:

```text
LightTouchMac/
  LightTouchMac/                 app source
  LightTouchMac.xcodeproj/
  build-support/
    release.py                  one build/release entry point
    sources.lock.json            immutable source/dependency selection
    recipes/                    dependency and helper builds
    profiles/                   firmware identity and recipe metadata
  tests/                        package and app acceptance
  licenses/                     notices and source provenance
  .build/                       ignored source staging, prefixes, intermediates
  dist/                         ignored app archive, source archive, inventories
```

A release should produce the app archive, checksums, exact source bundle, license notices, dependency inventory/SBOM, and a build record. SDKs remain external developer inputs subject to their licenses. Build caches should be disposable and keyed by inputs/toolchain, not trusted because a version-named stamp file exists. Start with repeatable source builds; do not claim bit-identical signed archives without separate verification.

**IPSW-only setup is plausible, but not implemented or proven.** The documented target is `iPod2,1`, iPhone OS 3.1.3, build `7E18`. No `.ipsw` archive was found in the inspected supplied source/firmware/research folders; the firmware assessment therefore traces current source and documentation, not an independently verified extraction or clean boot from an archive.

| Required material | What exists now | IPSW-only status |
|---|---|---|
| Root filesystem and kernelcache | NAND builder accepts IPSW-derived files; kernelcache is copied into the filesystem | Main firmware content is accounted for, but preparation depends on additional tooling/inputs |
| iBoot, device tree, other NOR contents | Existing prepared iBoot and NOR files; NOR builder accepts IPSW all-flash components | Need a complete, versioned preparation/provenance record for the exact archive |
| Boot ROM | Separate `bootrom_240_4` required by app and emulator | Not accounted for by the current IPSW workflow |
| NOR configuration | Builder requires a base NOR for IMG2/SysCfg/NVRAM | Must replace copied state with independently generated configuration where possible, or disclose an additional required input |
| NAND metadata | Builder copies all template pages outside its filesystem mapping | Must replace inherited pages with an explicit generated format; current output is not independent of the template |
| Guest integration | Authored GLES replacement, agent, media/input helpers, settings; existing modified guest components | Separate authored additions from Apple-derived or protection-related modifications and review each item's provenance/license |
| Toolchain | Legacy SDK and external image tooling | Developer dependency, not something an end user's IPSW supplies |

The key evidence is [build_nand.py's template copy](../imgtools/build_nand.py), [build_nor.py's base requirement](../imgtools/build_nor.py), and the app's [required asset list](../../LightTouchMac/LightTouchMac/LaunchOptions.swift). The setup guide itself says generation of NAND bookkeeping from first principles remains unsolved: [setup guide](../docs/ipod-touch-2g-setup.md). Copying fewer opaque template pages would not, by itself, establish their provenance.

There is a useful boot-ROM research lead: the current 3.1.3 path already starts at a later boot stage, yet still loads the separate ROM during initialization/reset. That makes the necessity of the ROM worth examining. It does not prove it can be removed: later calls, reset behavior, data dependencies and device initialization need accounting. Assess whether an independently implemented initialization path can meet the emulator's requirements; otherwise the product must describe additional user-supplied material honestly. This assessment does not implement or validate boot/protection bypasses.

**Make firmware preparation a local device-creation step, independent of application packaging.** The target user flow is: open Light Touch, select the supported IPSW, validate its identity and contents, prepare a local device, then boot it. Ship reviewed authored code and licensed dependencies; keep user firmware and generated Apple-derived images in the user's local storage.

The preparation result should be a versioned device-image manifest: device/build identity, hashes of every external input and prepared component, recipe version, guest-tool version/capabilities, and storage format. Give the complete asset set an identity, rather than identifying only the NAND. Validate the exact supported archive and expected components rather than its filename. Do not invent a trusted digest until a reference input has been verified.

Preparation should use temporary storage and publish a complete image atomically, with cancellation, disk-space checks and useful errors. Archive handling should bound extraction and reject paths outside its destination. Product preparation must run without developer paths, a shell setup session, Homebrew, or an assumed user-installed Python; a bundled importer/helper or self-contained runtime can reuse existing logic. Old HFS/image-tool compatibility must be tested on the supported host OS. Keep any recipe capable of running arbitrary developer scripts out of the consumer import interface.

The app already keeps device state in Application Support and retains each device's original base: [DeviceStateStorage.swift](../../LightTouchMac/LightTouchMac/DeviceStateStorage.swift). Extend that mechanism instead of replacing it. Preserve base/overlay pairs through app upgrades. Image recipe updates create new bases and explicit device migrations; they must not silently combine old writable state with a new firmware base.

Today, missing firmware produces an alert and quits: [AppDelegate.swift](../../LightTouchMac/LightTouchMac/AppDelegate.swift). `LTM_ASSETS=none` alone therefore is not a finished firmware-free product.

**User-supplied firmware reduces a major distribution risk, but “would that save us?” has no blanket yes.** This is a US-oriented issue assessment for planning, not a legal clearance opinion.

Copyright law reserves reproduction, derivative-work and distribution rights, subject to limitations. Moving Apple firmware out of your downloads removes that direct distribution from the proposed release. User possession of an IPSW does not itself authorize every use, and a delta/patch is not automatically free of Apple expression. [17 USC §106](https://www.copyright.gov/title17/92chap1.html#106)

Apple's available legacy iPod touch agreement limits licensed use to owned/controlled Apple-branded iPod touches and restricts redistribution, modification and decryption, subject to applicable-law/open-source exceptions. It does not expressly authorize desktop emulation. The located PDF is dated April 29, 2008: the exact agreement applicable to the 3.1.3 input remains to be established. [Apple legacy iPod touch SLA](https://images.apple.com/legal/sla/docs/ipodtouch.pdf)

Decryption and protection-related modifications create a separate §1201 question. Its interoperability exception has conditions; ownership alone is insufficient. Triennial exemptions do not generally authorize distribution of circumvention tools. The current mobile-device jailbreaking exemption concerns apps interoperating on the device, not an express general permission to emulate the entire OS on a Mac. [17 USC §1201](https://www.copyright.gov/title17/92chap12.html#1201), [37 CFR §201.40](https://www.copyright.gov/title37/201/37cfr201-40.html)

There is also a concrete source-provenance concern: the research iBoot tree includes [files marked confidential and proprietary](../../ipod2g-re/iBoot-master-7b9581e6c9e689354ec50d3c4ef9b10cf480c9cb/drivers/samsung/pke/AppleS5L8900XPKE-hardware.h), and [the PKE design note](../docs/ipod-pke.md) says that reference was consulted. This does not establish copying or infringement. It does mean independence cannot simply be assumed, and deleting the research directory would not answer the provenance question. Review relevant source/commit history and distinguish observed behavior and independently written implementations from copied expression. Keep the archive out of releases; do not destroy evidence.

QEMU is GPLv2, and the Mac app currently links its dylib directly. A public repository alone is not a license. No tracked license file was found for LightTouchMac. Plan for GPL-compatible licensing of the combined app, subject to ownership and dependency review, and provide corresponding source/build scripts for released binaries. Separate repositories do not change linkage. A subprocess architecture would require its own analysis, not provide an automatic exception. [QEMU license](https://www.qemu.org/docs/master/about/license.html), [GPLv2](https://www.qemu.org/license-gpl-2/), [GNU aggregation guidance](https://www.gnu.org/licenses/gpl-faq.en.html#MereAggregation)

Audit the complete native/guest dependency closure, including static libraries, Swift packages, helper binaries and FFmpeg patches. The existing FFmpeg notices are a useful start, not a complete inventory. Have copyright/technology counsel assess the actual importer, protection-related changes, source provenance, and applicable Apple agreement before treating this design as cleared for public release.

The [existing public release page](https://github.com/samhenrigold/qemu-ios/releases/tag/ipod-touch-2g-2026.08) advertises an app containing the device and separate firmware images. Its prose was checked; binary asset contents were not downloaded or audited. Moving future releases to user-supplied inputs does not address past distribution. Include prior releases and their source fulfillment in the review; no published artifacts were altered here.

**Do the migration in this order, with a concrete completion criterion for each step.**

1. **Preserve and inventory.** Record dirty work and unpublished commits; inventory source, input, generated image and state ownership. Complete when every current release input has a source/provenance record, including loose utility scripts, SDKs and guest binaries. Avoid bulk moves or deletions until recovery is proven.
2. **Close the build graph.** Add the product lock manifest and regenerate dependencies/tools from declared sources. Compile the helper directly; build the app from exported QEMU artifacts. Complete when a fresh checkout in arbitrary paths builds without the old prefix, old app, personal scratch directories or prebuilt guest files.
3. **Prove firmware input independence.** Account for the boot ROM, NOR configuration, NAND metadata, preparation tooling and every guest modification. Complete only when a disposable workspace can create the device from the explicitly advertised inputs plus reviewed shipped code, without reading the old firmware/research folders. Start with an inventory of missing inputs; do not label the product IPSW-only prematurely.
4. **Implement consumer setup.** Connect local preparation and image manifests to first launch and retained device state. Complete with wrong-input, interrupted-import, cold-boot, shutdown, persistence, erase and upgrade behavior demonstrated on a clean supported Mac.
5. **Consolidate release delivery.** Assemble a fresh firmware-free bundle, verify its full inventory and dynamic-library closure, sign/notarize, and produce matching source/notices/build records. Review historical releases and provenance with counsel. Retire the older app pipeline and redundant wrapper once their required functionality and history are preserved.

No boot, firmware-generation or release-validation tests were run for this assessment. Existing tests and documentation support the proposed work; they are not evidence that this proposed release pipeline or IPSW-only workflow already works.
