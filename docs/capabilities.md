# iPod touch 2G / iOS 3.1.3 status

Updated 2026-09-04. This is the current project entry point; the reverse-engineering
notes in `ipod2g-re` mostly describe **2.1.1 / 5F138**, not the current **3.1.3 /
7E18** target. Do not reuse kernel addresses or the 2.1.1 USB gate patch on 3.1.3.

## What the directories contain

| Directory | Role |
| --- | --- |
| `LightTouchMac` | Native AppKit frontend, installation/library UI, snapshots, packaging. |
| `qemu-ios` | Emulator, guest helpers, image tools and regression harness. |
| `qemu-ios-files` | Local firmware, prepared NAND images, experiment overlays and evidence. Not a source repository. |
| `qemu-ios-deps12` | Reusable static USB/client dependency prefix. Its build script depends on locally cached source/toolchain files. |
| `ipod2g-re` | Kernel/kext/MBX reverse-engineering workspace; check the firmware version of every note. |
| `usbmuxd-qemu` | Required sibling fork carrying USB between the emulator and libimobiledevice. |

The stock base images are read-only inputs. Guest writes belong in a separate
`nandrw` overlay. Never reuse an overlay with a different base image.

## Work that had landed by August 12

- Real 3.1.3 apps install over USB using the prepared AppSync image. A decrypted
  ARMv6 executable and compatible frameworks remain prerequisites; metadata
  alone does not establish that an app actually runs.
- GLES 1.1 renders through `contrib/it-gles/mbxshim.c` and `hw/arm/gles-host.c`.
  **PVRTC/paletted textures and VBO uploads are implemented.** The old plans
  describing them as absent are obsolete. RGB565 drawable handling and texture
  completeness fixes also landed. Historical screenshots recorded Angry Birds
  rendering; this is distinct from verifying every level of every game.
- Guest shutdown uses `contrib/it-halt/ithalt` through SSH in the Mac app.
  `system_powerdown` still drives a SpringBoard gesture and depends on a
  responsive/unlocked UI. A host process exit is not proof of a guest unmount.
- Bluetooth firmware startup has UART/DMA, minidriver-announcement and reset
  handling. This does not implement a working Bluetooth radio or remote peers.
- App work included orientation/tilt, sidebar updates, install progress,
  lock/backlight behavior and first-run packed-NAND extraction. Timezone writes
  were moved out of process after an in-process heap-corruption failure.

## September reliability fixes

- GLES pixel format/type and alignment validation precedes host calls. PACK
  and UNPACK are independent. Query output cardinalities are explicit; compressed
  format queries describe the emulator's decoders rather than host GL formats.
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

## Verification

Device-free checks (no firmware modifications):

```sh
python3 tests/ipod/test-gles-boundaries.py
python3 tests/ipod/test_fmss_persistence.py
python3 tests/ipod/test_regress.py
python3 tests/ipod/test_launch.py
python3 tests/ipod/test_pmu_shutdown.py
```

The GLES check compiles actual boundary handlers under ASan/UBSan. The NAND
check injects host-I/O failures into the real C write path. App filesystem and
packaging checks live in the LightTouchMac repository.

Run the actual device checks with a **new output directory** and a real IPA:

```sh
python3 tests/ipod/regress.py \
  --qemu build-native14/qemu-build/qemu-system-arm \
  --base-nand "$HOME/Developer/qemu-ios-files/nand-ultimate" \
  --checks boot,afc,usbtcp,appinstall,applaunch,gles,persist,fsck \
  --ipa /absolute/path/to/decrypted.ipa \
  --out /tmp/ipod-check-new-run
```

The harness writes only its disposable overlay. It needs localhost sockets,
USB client tools, built guest helpers and a NAND containing SSH for native
launch/shutdown checks. Inspect SKIPs: exit zero alone does not mean every
optional check ran. Run one emulator at a time when judging touch or timing.

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

- **Native halt versus power-off remains unresolved.** `ithalt` reaches the
  7E18 kernel's intentional terminal halt loop without emitting a PMU power-off
  command. The clean-shutdown persistence check therefore cannot pass; successful
  full-volume fsck is separate evidence. See [native halt investigation](ipod-native-halt.md).

- Host GLES state still uses a global context. Demonstrate a real affected
  application before undertaking per-guest-context isolation (old plan 05).
- The filesystem's inferred erase spans multiple files. I/O errors stop the
  session; fully transactional recovery from interruption mid-erase would
  require block journaling. No claim of universal power-loss recovery is made.
- Snapshot resume is opt-in. Keep save/restore and snapshot/flash consistency
  coverage when changing device migration state.
- Idle wake, self-reboot, debugger attach and remote-name DNS have historical
  open notes. Those 2.1.1 observations are not a current 3.1.3 failure list;
  reproduce each before changing hardware models.
- `docs/app-compatibility.md` is an August 5 survey. Its static import/missing-slot
  lists predate later GLES implementation and do not represent current coverage.
- The salvage directory under `qemu-ios-files/worktree-salvage-2026-08-03` holds
  old uncommitted experiments. Preserve it as evidence; do not blindly apply
  those diffs over subsequent fixes.
