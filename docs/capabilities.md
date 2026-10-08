# iPod touch 2G / iOS 3.1.3 capabilities

Updated 2026-09-28. Project status is LightTouchMac `docs/STATUS.md`; this file is the iPod board's
capabilities, limits and checks (the repository README is the map). The reverse-engineering
notes in `ipod2g-re` mostly describe **2.1.1 / 5F138**, not the current **3.1.3 /
7E18** target. Do not reuse kernel addresses or the 2.1.1 USB gate patch on 3.1.3.

## What the directories contain

| Directory (under `~/Developer`) | Role |
| --- | --- |
| `LightTouchMac` | The native AppKit app (sidebar of devices, one helper process per device), the Swift preparer `firmwarekit`, the app gates and the product build. Its `docs/STATUS.md` is the project status. |
| `qemu-ios` | This repository: both machines, guest helpers (`contrib/`), the Python image pipeline (`imgtools/`, the preparer's test oracle) and the regression harnesses (`tests/`). Branch `ipad1` carries both boards; `ipod_touch_2g` is the public iPod-only main line. |
| `qemu-ios-files` | Local firmware, prepared devices, images, overlays and evidence. Not a source repository; `nand-current` is the shipping iPod image. |
| `usbmuxd-qemu/usbmuxd` | The usbmuxd fork (branch `qemu-zlp`) that carries USB between the emulator and libimobiledevice; required for every USB-side check. |
| `ipod2g-re` | Kernel/kext/MBX reverse-engineering workspace; check the firmware version of every note. Its `OldSDK` holds the iPhoneOS 3.1.3 SDK the guest helpers build against. |
| `qemu-ios-deps12` | An old static dependency prefix. The product build no longer needs it; a few guest tests still default `OPENSSL_PREFIX`/`PATH` to it. |

The stock base images are read-only inputs. Guest writes belong in a separate
`nandrw` overlay. Never reuse an overlay with a different base image.

## Current host/guest interfaces

The [guest agent](../contrib/it-agent/README.md) serves bounded cp15 RPCs for
binary file transfers, commands, native app/lock state, clipboard, typing and
foreground UI inspection. It has no guest-network listener. Cancellation and
reset invalidate pending sessions; uncertain mutations are not automatically
replayed. There is no SSH path: devices built from an IPSW get no shell
(`ipod/from-ipsw.md`), the harness runs no `guest_ssh`, and the app's guest
commands are agent ops.

[Typed machine configuration](configuration.md) is being introduced incrementally.
`audio-hw=auto|on|off` is the first migrated hardware-presence option; explicit
values override its legacy environment alias. The remaining `IT_*` migration is
unfinished. See machine help and the configuration document for tested semantics.

The built-in HTTP proxy now supports live stock Weather search/forecasts, host
TLS for old clients, and optional Internet Archive replay. Stocks has native
synthetic quote/chart verification, not a live-data integration. See the
[service protocol and limitations](stock-service-protocol.md).

## Work that had landed by August 12

- Real 3.1.3 apps install over USB using the prepared AppSync image. A decrypted
  ARMv6 executable and compatible frameworks remain prerequisites; metadata
  alone does not establish that an app actually runs.
- GLES 1.1 renders through `contrib/it-gles/mbxshim.c` and `hw/arm/gles-host.c`.
  **PVRTC/paletted textures and VBO uploads are implemented.** The old plans
  describing them as absent are obsolete. RGB565 drawable handling and texture
  completeness fixes also landed. Historical screenshots recorded Angry Birds
  rendering; this is distinct from verifying every level of every game.
- Guest shutdown used `contrib/it-halt/ithalt` through SSH in the Mac app;
  since then it is the agent's halt op (no shell), and the app's Stop is a
  flush plus hard halt. A host process exit is not proof of a guest unmount.
- Bluetooth firmware startup has UART/DMA, minidriver-announcement and reset
  handling. This does not implement a working Bluetooth radio or remote peers.
- App work included orientation/tilt, sidebar updates, install progress,
  lock/backlight behavior and first-run packed-NAND extraction. Timezone writes
  were moved out of process after an in-process heap-corruption failure.

## Kernel console

`-M 'iPod-Touch,...,boot-args=amfi_allow_any_signature=1 cs_enforcement_disable=1 serial=3 debug=0x8'`
enables XNU serial output on 7E18. The machine property is the only input (no
`IT_BOOT_ARGS` environment), for the early iBoot handoff as well. An
empty value disables injection; values longer than 255 bytes and changes after
startup are rejected. Add `-v` for
verbose text on the panel; serial logging works without it. The earliest kernel
banner precedes serial initialization, but driver startup and `BSD root:` are
captured. (The `serial-console` check of the retired `tests/ipod/regress.py` verified those outputs; see git history at 5508b504b8.)

The local asset launcher `qemu-ios-files/ios3/run-ios3.sh --console` enables both
serial and verbose boot.

## September reliability fixes

The September 5 continuation, frontend changes, verification evidence, and
remaining master-plan priorities are recorded in [UI reliability](archive/ipod-ui-reliability.md) (archived).

- GLES pixel format/type and alignment validation precedes host calls. PACK
  and UNPACK are independent. Query output cardinalities are explicit; compressed
  format queries describe the emulator's decoders rather than host GL formats.
- Bulk NAND writes no longer stop at 512 pages. DMA reads are bounded to RAM,
  and invalid scripts stop the VM instead of silently dropping file data.
- NAND persistence errors stop the VM and latch a host storage failure. Failed
  stores are not inserted as successful physical-cache writes. The app displays
  the failure and refuses resume/snapshot operations on that session.
- Shutdown success requires the guest PMU power-off signal in the app and a
  guest-origin QMP `SHUTDOWN` event in command-line checks. An SSH disconnect,
  `sync`, socket disappearance or exit code zero alone cannot pass it.
- Snapshot promotion uses a checked atomic rename. Failure reaches the existing
  clean-shutdown fallback. Overlay writes after a snapshot invalidate restore;
  the old ten-second tolerance is gone. Unreadable metadata fails closed.
- Packaged NAND identity is content-based. Existing devices stay pinned to their
  original extracted image and overlay; an explicit device reset adopts the new
  bundled image. Ambiguous legacy state is not silently paired with another base.
- Packaging checks the complete native load closure and actual rpath resolution.
  The macOS 14 build retains CGL, CoreAudio and Wi-Fi/slirp without newer Homebrew
  runtime libraries. The build recipe is in `LightTouchMac/scripts/build-package-native.sh`.
- Regression checks now use the IPA's exact bundle ID and native SpringBoard
  lock/foreground state. Installation failures are no longer automatically XFAIL
  after AFC. Filesystem checks reconstruct the full HFS+ volume and require
  `fsck_hfs` exit zero; they do not whitelist bitmap/header damage.

Native shutdown now uses launchd-coordinated `reboot2`, and the PMU accepts
its actual standby command without host-side gesture arming. The watchdog
recognizes the exact immediate-reset command instead of treating ordinary
keep-alives as resets; launchers no longer suppress guest resets. Generated
NAND images discard the previous boot's temporary physical mapping on reset,
while physical-image mode retains its programmed pages. This fixes the native
reboot that entered recovery with an invalid HFS signature.

The `boot,restart,persist,fsck` run passed with resets enabled: a 65,535-byte
marker survived warm reset, another survived clean shutdown and cold boot,
and all 1,835,008 HFS allocation blocks passed fsck. See
[native shutdown evidence](ipod/ipod-native-halt.md).

## Verification

Device-free checks (no firmware modifications):

```sh
tests/gate.sh --quick    # every tests/slice/*.c (GLES boundaries, FMSS persistence, PMU shutdown, WDT reset, ...) and tests/*/*-test.sh
```

The GLES check compiles actual boundary handlers under ASan/UBSan. The NAND
check injects host-I/O failures into the real C write path. App filesystem and
packaging checks live in the LightTouchMac repository.

Device checks (boots of prepared devices) are LightTouchMac's
`swift run --package-path tests/sessions sessions single <BASE>` and its Release
plan's prepare matrix. The former `tests/ipod/regress.py` harness is retired;
see git history at 5508b504b8.

On September 4, boot, AFC byte integrity, USB TCP, exact IPA installation and
foreground launch, and the GLES test pattern passed against `nand-ultimate`.
Both the pristine and written full 7 GiB volumes passed read-only fsck. Snapshot
save/restore and truncated-snapshot rejection also passed. Per-run results and
screenshots are kept in the harness output directory.
The native package passes macOS 14 deployment-target/load-closure and ad-hoc
signature checks. A fresh-state launch with only system tools on PATH unpacked
the bundled NAND and initialized USB and GLES. That disposable launch was
forcibly stopped; it is not a clean-quit test. Execution on an actual macOS 14
machine still needs separate validation.

## Remaining work and deliberate limits

- Media is enabled for ordinary Light Touch launches; standalone QEMU still uses
  the explicit media environment switches. Active decoder/graphics snapshot
  state is unfinished. Packaged-app playback and existing-device component
  upgrades are validated. AAC-LC, HE-AAC,
  MP3 and ALAC reach native audio; Spore's original MPEG-4 intro and progressive
  H.264 I/P video now display with controls and a status bar. Complete reference
  clips pass sample/pixel checks, including playback after warm reset. NV12
  LCD scanout supports unscaled and polyphase-scaled video. H.264 also handles multi-slice
  pictures, repeated weighted reference entries, and later-slice reordering or
  subsets of references, bounded replay when later slices introduce references,
  mixed I/P slices, constrained intra prediction, nonzero deblocking offsets,
  and CAVLC I_PCM alignment. Hardware filter rounding,
  additional transforms/formats, H.264 B/field pictures and per-slice DMA,
  and active-decoder snapshot state remain unfinished. Physical 7E18 reference
  decoding confirms mono HE-AAC v2 behavior; both its LC and HE program outputs
  now match complete emulator offline captures within codec rounding differences.
  See [media evidence and runnable checks](research/ipod-media.md).


- The native macOS GLES bridge isolates guest contexts/sharegroups and clears
  them on machine reset. The legacy EAGL backend still needs equivalent
  context isolation and reset cleanup.
- The filesystem's inferred erase spans multiple files. I/O errors stop the
  session; fully transactional recovery from interruption mid-erase would
  require block journaling. No claim of universal power-loss recovery is made.
- Light Touch does not resume on launch, and Sam decided on 2026-09-28 to delete
  its quit-with-resume snapshot code (LightTouchMac `docs/sweep/PLAN.md`, S3).
  Live graphics/decoder state still has documented snapshot restrictions. Keep
  save/restore and snapshot/flash consistency coverage when changing device
  migration state.
- Idle wake, self-reboot, debugger attach and remote-name DNS have historical
  open notes. Those 2.1.1 observations are not a current 3.1.3 failure list;
  reproduce each before changing hardware models.
- `docs/archive/app-compatibility.md` is an August 5 survey. Its static import/missing-slot
  lists predate later GLES implementation and do not represent current coverage.
- The salvage directory under `qemu-ios-files/worktree-salvage-2026-08-03` holds
  old uncommitted experiments. Preserve it as evidence; do not blindly apply
  those diffs over subsequent fixes.
