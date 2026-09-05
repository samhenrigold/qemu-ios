# Light Touch / emulator continuation — September 5, 2026

This records the interrupted UI/reliability workstream and reconciles it with
the supplied hardware-fidelity master plan (baseline `f6eb8c4761`). The plan is
research guidance; its proposed mechanisms and historical failures still need
confirmation against the current 7E18 guest.

## Implemented

| Request | Result | Commit |
| --- | --- | --- |
| Verbose boot | Repair CHRP partition bounds/checksums and inject arguments before XNU parses them. Release 7E18 iBoot ignores NVRAM arguments during normal boot; a signature-checked handoff edit supplies them. Unknown iBoot images are left untouched. | QEMU `fa1d82df4c` |
| Coldplay rendering | Fault guest upload pages into memory before host GLES reads texture and buffer data, respecting unpack alignment and packed pixel formats. | QEMU `e4f8e0c577` |
| Home/button responsiveness | Shorten frontend Home hold and preserve at least 100 ms of guest VM time when host down/up events arrive together. | QEMU `12e0d3d439`, Light Touch `a079cfb` |
| Zoom and menus | Fit or integer pixel scale; remove physical-size heuristics and resume/snapshot menu commands. | Light Touch `a079cfb` |
| Preparing media / Restart SpringBoard | Poll the SpringBoard service instead of a fixed delay; show operation state and gate duplicate restarts. Reload the launch job safely when upgrading media components. | Light Touch `a079cfb`, `1ffbef2` |
| Lock button | Remove legacy `SBDontLockEver` and `SBDisableCABlanking` image settings. Migrate existing devices while SpringBoard is stopped, preserving unrelated preferences and reloading the job on failure. | QEMU `576523b2ec`, Light Touch `1ffbef2` |

The lock failure was reproduced with valid kernel button interrupts. It was
caused by those guest preferences, not a missing power-button IRQ. The image
baker now removes the flags. The historical `qemu-ios-files/apps/patch-appsync.sh`
was also corrected locally; that files directory has no Git repository.

## Verification

- Sanitizer-backed QEMU checks: `test_early_boot_args.py`,
  `test_nvram_boot_args.py`, `test_ui_buttons.py`, `test_gles_upload.py`, and
  `test_gles_surface.py` in `tests/ipod`. The GLES surface check requires a
  working macOS OpenGL context.
- Light Touch: `scripts/test-zoom.py`, `tests/check-rotation.py`,
  `tests/check-media-components.py`, and `tests/check-media-reload.py`.
  These cover scale transitions, rotation, XML/binary plist preservation and
  idempotence, corrupt-input rejection, binary SSH input/cancellation, and
  launch-job restoration after a failed preferences write.
- Debug and Release builds, QEMU executable/dylib rebuild, and complete
  packaged dependency/deployment-target checks passed. The local app is
  ad-hoc signed; it is not a notarized release.
- Guest boot evidence shows `-v` in iBoot's kernel command line and reaches
  SpringBoard. Coldplay gameplay was rendered after the upload fix.
- Guest lock checks passed at 100, 300, and 800 ms holds; Home woke to the lock
  screen and the scripted swipe unlocked. The packaged frontend separately
  passed automatic migration, Lock/display-off, Home/wake, and confirmed PMU
  shutdown. Initial media migration took about eight seconds in that run.
- Ten restart cycles on the current engine recovered, including five with
  native OpenGL compositing explicitly enabled.

Local evidence includes `/tmp/it-resume-lock-fixed.log`,
`/tmp/it-resume-restarts.log`, `/tmp/it-resume-native-restarts.log`,
`/tmp/it-coldplay-fixed.png`, and `/tmp/it-resume-app-home/app.log`.
Temporary paths are session evidence, not durable regression fixtures.

## Remaining reliability work

Installing the 58 MiB Coldplay IPA and immediately restarting SpringBoard
reproduces a crash loop on the current engine. SSH remains responsive. The new
crash is in CoreFoundation preferences called by BluetoothManager, distinct
from the historical recursive QuartzCore image-binding crash.

The BluetoothManager framework's `Info.plist` is 510 bytes both before and after
installation. Before installation it is a valid binary plist with identifier
`com.apple.BluetoothManager`; afterward its bytes match the IPA at compressed
file offset 2,899,968 and `Payload/Coldplay.app/Themes/TTRDJ.ttrTheme/sub_theme2/bg-matrix2.png`
at offset 59,907. SHA-256 values:

- Before: `1916a889d004ba7c9bea2a942f0fb6645e63867ffb22594c2322cd8268036321`
- After: `55147f9d33aa5e7caf0b8a05f6f163410976e042a00f8ce88ea911dd5cc844ac`

The intact file is at base `cs2/1126.page`, offset zero. There is no replacement
at that path in the test overlay. `FMSS_RTRACE=1` records `RP cs=2 page=1126`:
`fmss_recall_physical` supplies a session-programmed page instead of the live
system file. The generated image's synthetic free-space metadata and its
session physical mappings allow installation writes to alias system data.
Cold boot discards those mappings, explaining the apparent restart remedy.
Simply invalidating the cache would instead break reads of the newly installed
data; it is not a safe fix. The existing `FMSS_USEDSPARE=1` diagnostic still
reproduced the crash and is not a remedy.

**This corruption is not fixed.** The next storage change needs a coherent FTL
allocation/mapping model for generated images (or a correctly populated physical
image), so live base pages and newly allocated pages cannot share coordinates.
Do not paper over it with a Bluetooth patch, repeated respring attempts, or a
claim that the GLES upload fix resolved it.

The new `respring` check in `tests/ipod/regress.py` runs after installation and
before a cold restart, requires the SpringBoard service to respond within 45
seconds, and saves the latest guest crash report on failure. Select
`--checks boot,appinstall,respring --ipa <IPA>` with the usual local firmware,
QEMU and usbmuxd paths. `test_respring.py` checks that a timeout or unrelated
successful reply cannot pass. Commit: `db7b34fdfe`.

Reproduction evidence: `/tmp/it-resume-install-files.log`,
`/tmp/it-blitz-spore-38758/bt-before.plist` and `bt-after.plist`,
`/tmp/it-resume-install-nand.log`, and
`/tmp/it-blitz-spore-39025/device/qemu.log`. The older QuartzCore recursion remains
unattributed; the demonstrated storage corruption is capable of affecting more
than one framework, but that does not prove the older crash had the same cause.

Explicit locking and waking do not prove native idle sleep/wake. Test auto-lock
with the demo-card workaround disabled and without a forced-brightness override,
then inspect PMU masks/events, CPU idle state, and the panel wake sequence.

## Master-plan priorities

1. Fix generated NAND allocation aliasing and rerun install/respring plus
   post-install file-integrity checks before expanding peripherals.
2. Validate PMU ADC completion and masked event delivery. This provides a shared
   foundation for battery reporting, headphone detection, and idle wake.
   The plan's shutdown-stall hypothesis predates the current confirmed PMU
   shutdown path; do not treat that historical stall as still demonstrated.
3. Trace Mikey headset detection, then implement I2S RX and microphone routing.
   The guest must recognize a headset microphone before host input is useful.
4. Trace Settings-driven Wi-Fi joining and its event order; avoid treating
   successful host networking as proof that the Settings join flow is correct.
5. Extend the now-working early boot-argument path to verify serial console
   output. NVRAM editing alone is insufficient on release 7E18 iBoot.
6. Add distinct instance identities/MACs and isolated state for two-instance LAN
   tests, then replace individual hardware shortcuts with trace-backed behavior.

Bluetooth peers and TV-out remain deferred as in the supplied plan. For
guest/host communication, prefer the existing QMP and usbmux services with
explicit readiness, operation results, and diagnostic evidence before adding
another transport.
