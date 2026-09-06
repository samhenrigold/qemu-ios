# Emulator and Light Touch plan progress

The workstream follows the supplied hardware-fidelity plan, with reproducible
storage corruption promoted ahead of new peripherals. Each completed item needs
both a focused regression and relevant guest evidence. Bluetooth peers and
TV-out were explicitly deferred by that plan.

| Track | Status | Acceptance evidence still needed |
| --- | --- | --- |
| Generated NAND integrity and restart stability | Complete for the reproduced failures: free-pool bounds, FMSS completion, VIC fixes and paced TV-out IRQs | Twelve alternating Coldplay/Spore install/respring cycles; unchanged system file; guest shutdown; cold boot with both apps; full-volume read-only fsck passed |
| PMU ADC and masked event IRQ | Complete | Ten-bit results, settling vs conversion, mask/read-to-clear and GPIO tests pass; real 7E18 boot, three lock/wake cycles and native shutdown pass |
| Battery controls | Core and Light Touch level/charging/drain controls implemented | 20/60 percent cold calibration, full-voltage estimate, runtime off/on/auto and native shutdown pass; preserve guest filtering delay |
| Headset/Mikey detection | Deferred at user request (2026-09-05) | Plug/unplug and headset button traces; correct guest routing |
| Microphone/I2S RX | Deferred at user request (2026-09-05) | Deterministic input tone captured by the guest, then host microphone recording |
| Native idle sleep/wake | Untethered manual/automatic lock and Home/Power wake verified without brightness override | Deeper suspend paths still need acceptance |
| Kernel serial console | Complete: explicit machine arguments, live console regression and Light Touch control build pass | Include the updated control in the final package verification |
| Settings Wi-Fi join | Deferred at user request (2026-09-05) | Manual join without known-network seed or alert loop; DHCP and traffic |
| Two-instance LAN | Deferred at user request (2026-09-05) | Separate identities, MACs and state; bidirectional traffic between guests |
| Hardware shortcuts | FMSS completion and TV-out frame timing fixed; VIC daisy-chain defects fixed; DSI panel reply queue implemented | PKE, NOR persistence, FMSS erase, remaining LCD/TV-out status and AES matching review |
| Light Touch integration | Original UI fixes committed | Package and verify the resulting hardware controls and readiness/recovery paths |
| Bluetooth peers / TV-out | Deferred per supplied plan | No implementation claimed |

See [UI reliability](ipod-ui-reliability.md) for the original fixes and the
storage reproduction. Temporary `/tmp` paths there identify local evidence;
they are not dependencies of the committed tests.

## User priority update (2026-09-05)

Finish the frame-polling crash fix, non-quitting Power Off/On, sleep/off visuals,
and foreground-app subtitle first. Then implement HTTP proxy configuration with
WaybackProxy integration, followed by screen capture/recording, Live Text and
optional touch/finger overlays (using WireView as a reference), then resume the
remaining active plan. Headset/microphone, Settings Wi-Fi join and two-instance
LAN are deferred by explicit user request.

## Proxy and capture follow-up

- Frame-poll startup crash fixed and sanitizer-tested (`074da22b44`).
- Non-quitting Power Off/On, sleep/off visuals and foreground subtitle built
  and native power cycles verified (`460d062`, `7c3cdae801`).
- HTTP proxy: built-in direct HTTP and optional dated Internet Archive replay implemented.
  Native NSURLConnection through guestfwd returned HTTP 200 for an unresolvable
  origin using a controlled upstream fixture; reversible configd preferences
  passed on/on/off/off. Host sanitizer tests cover HTTP framing and tunnels.
  No external proxy installation is needed. Host archive acceptance returned a
  September 2009 example.com page; the subsequent native guest request received
  the archive's HTTP 429 response (rate limiting, not a local routing failure).
  Modern TLS termination remains a later phase.
- Native screenshots, inline Live Text and video recording implemented. Real
  H.264 encode/decode tests cover colors, rotation margins, dimensions and timing;
  a 68-second UI recording is playable. Guest audio recording is now verified below.
- Finger dots are 44 AppKit points, soft white/gray with shadow and fast release
  fade, suppressed asleep/off. Their layers are siblings of the framebuffer to
  isolate opacity. Sleeping uses the user's Sleeping.caar animation.

### Proxy and overlay corrections verified

- No Proxy tolerates Safari's stale proxy connections while configd applies
  settings. Archive replay follows original HTTPS redirects on the host and
  rewrites absolute HTTPS links only in text resources; binary data is unchanged.
- Archive requests share a paced process gate, Retry-After cooldown and bounded
  date/URL cache. Loopback ASan/UBSan tests cover compressed HTML, redirects,
  cache hits during cooldown, parallel 429 suppression and switching off.
  Live Archive availability is not claimed fixed; explicit HTTPS URLs still
  require a future TLS bridge.
- Release build passed. Isolated UI inspection showed the framebuffer visible
  after touch release, inline text recognition, and the animated Sleeping asset.

### Console and capture toolbar follow-up

- Routine SDIO traces require IPOD_SDIO_TRACE=1. GLES first-slot diagnostics and
  framebuffer readbacks for diagnostic color averages require IT_GLES_VERBOSE.
  Normal rendering no longer stalls on the diagnostic offscreen glReadPixels.
  GLES surface/upload regressions and native builds passed.
- guestfwd merges stderr into its socket. The HTTP helper now prevents diagnostics
  from corrupting HTTP status/framing; merged-stream failure regression passed.
  Native NSURLConnection fetched both example.com and neverssl.com with HTTP 200
  through direct proxy, then neverssl.com with HTTP 200 after disabling it.
  Evidence: /tmp/it-proxy-direct-native-v2.log. Earlier host NeverSSL TCP timeouts
  were transient. Modern HTTPS/NYT compatibility still needs the TLS bridge.
- Screenshot, Record/Stop, and Live Text are default/customizable toolbar items;
  Copy Screen and Finger Dots are also customizable. Copy Screen is Shift-Cmd-C.
  A native VisionKit fixture using the production view verified that the corner
  Stop Analyzing Image button removes the complete Live Text overlay.

### TLS bridge acceptance checkpoint

The reproducible `test_webproxy_tls_guest.py` passed on a fresh 7E18 overlay:
untrusted SHA-1 RSA chain rejected; guest-local CA added using securityd's
user-domain API; TLS 1.0/AES128-SHA HTTP 200; CA removed; chain rejected again.
The helper needs the period-correct `modify-anchor-certificates` entitlement,
not the modern prefixed name. Evidence: /tmp/it-proxy-tls-native-v4.log.
Production CONNECT termination and automatic per-device CA management remain.

### Built-in TLS bridge implemented

CONNECT now terminates legacy guest TLS locally and fetches HTTPS using verified
system libcurl. A per-device CA is generated atomically in private state; the app
installs/removes only its public certificate in the guest through securityd.
The Mac trust store and base NAND are unchanged. The helper bundles OpenSSL
statically. Native guest HTTPS returned HTTP 200 for example.com and NYT. Offline
TLS sanitizer tests cover concurrent CA initialization, trust/name validation,
HTTP errors inside TLS, and rejection of an untrusted upstream. Release app built.
This completes the TLS transport step; modern WebKit feature gaps remain.

### Guest agent transport and native operations

- Added bounded cp15 RPC queues, strict base64 framing, memory-copy error checks,
  lease recovery, host cancellation, and reset invalidation. Tokens identify
  sessions; they do not secure the tunnel against other guest processes.
- `it_agent` retains the explicit-UTI clipboard pumps and adds ping, exec, atomic
  put/get, settime, launch/frontmost/lockstatus, kill, and launchd halt requests.
  Child process groups have bounded output and a timeout; command execution
  keeps clipboard/heartbeat ticks running. Guest receive pages are touched before
  host debug writes, which cannot fault in demand-zero pages.
- Native acceptance passed binary stdin/files, root shell execution, exit status,
  host-clock correction, and Settings launch/foreground detection. Evidence:
  `/tmp/it-agent-native-v4.log`. Protocol and operation ASan/UBSan tests pass.
- Added QMP and embedded frontend APIs. Acquired references protect frontend
  calls that overlap machine cleanup. Light Touch's command dispatcher builds
  and its isolated Swift check passes concurrent result routing, cancellation,
  and absent-agent fallback. It never retries a submitted mutation through SSH.
- Baked a separate `nand-agent-v2` candidate, removed its old clipboard launch
  job, fixed daemon/job ownership, and passed HFS validation. The existing
  `nand-ultimate` remains untouched. Baked launchd/restart acceptance is ongoing.
- Remaining Track A: typing injection, UI tree inspection, ranged reads,
  snapshot-load rekeying, and completing image/frontend rollout.

### Native typing and UI inspection

- SpringBoard's DYLD_INSERT_LIBRARIES is inherited by launched apps on 7E18;
  the minimal native probe logged separate SpringBoard and Settings PIDs.
- Added a bounded foreground-process route with expiring request cookies. The
  injected library handles type/backspace/uidump on the main run loop, without
  signal overrides or temporary IPC files. Root and UI replies cannot complete
  one another's requests. A non-consuming key check avoids idle SBS IPC polling.
- Native Notes and an installed Harness UITextField passed Unicode insertion,
  deletion, physical host keys, and UI-tree/text inspection. Bulk input uses the
  focused control's insertText: API; treating a paragraph as one keyboard key
  produced an oversized autocorrection prompt and was corrected. Repeatable
  acceptance: `test_agent_guest.py --typing`, /tmp/it-typein-regression.log.
- Command-agent review artifact: `build-native14/Light Touch-agent.app`, signed
  and verified, contains the validated agent-only NAND candidate. Typing rollout
  into the baked image and existing devices is the next step.

### Guest agent rollout completed and snapshot work underway

- Baked typing/command-agent launchd recovery passed, including Notes and Harness
  Unicode, physical keys, Spotlight, discarded unfocused keys and locked input
  rejection (`/tmp/it-agent-unlock-native.log`). SpringBoard focus IPC runs on a
  worker; synchronous queries from its main thread deadlocked its own service.
- Existing-device provisioning preserves launch configuration, replaces the old
  clipboard job and starts command service immediately. A real isolated Light
  Touch upgrade passed; the second launch reported media already current.
  Release build and dispatcher/configuration checks passed. The old default
  NAND remains intact; `nand-agent-v2` is the separately validated candidate.
- Native snapshot restore confirms the agent reclaims its unsaved session and
  corrects the guest clock. Added AMC, I2S and CS42L58 VMState in separate commits,
  each with a home-screen save/restore check. Active audio playback acceptance
  is still pending; a home-screen restore does not establish that result.
- SDIO now saves sparse firmware memory and partially consumed queued frames,
  in addition to registers, association and timers. Dropping those would lose
  control replies and interrupt masks. Bounded serialization sanitizer checks
  pass; fresh HTTP requests through guest Wi-Fi returned 200 before and after
  restore (`/tmp/it-sdio-snapshot-native.log`). Wi-Fi restore is no longer XFAIL.
- USB reconnection, Bluetooth UART state, H.264 bit-reader state and host video
  decoder cleanup are committed. Native restore passed fresh USB pairing and
  Wi-Fi HTTP requests. Saves with live host GL objects are refused; Light Touch
  reports that reason and binds snapshots to the loaded emulator build and NAND.
- Active audio restore remains unverified: the current graphics engine leaves
  three live GL contexts in the software-compositor test image, and the save
  guard correctly refuses it. Replacing the stale baked engine did not remove
  this limitation (`/tmp/it-audio-snapshot-current-engine.log`). Active video
  restore also needs acceptance; no full host GL serialization is implemented.
  The user's removal of Resume on Launch supersedes the plan's default-resume
  proposal; do not restore that menu feature.

### Regression harness follow-through

- The default IPA is now the repository's Harness.ipa, replacing a private app
  path. The default guest-agent check verifies readiness, ping, shell arithmetic
  and an exact 70 KiB binary round trip. Device-free checks reject operation
  failures and successful responses containing corrupt bytes. The remaining
  Harness graphics/audio integration is now implemented: app install/launch,
  GLES, agent and stereo audio join the default tier. GLES uses the baked
  renderer unless `--stage-gles-shim` is requested, with a Harness fallback when
  GLTest is absent. Native GLES passed. The new audio check detects a real silent
  host output despite nonzero raw I2S samples. This was traced to dock-ID ADC
  channel 3 falsely reporting a Line Out accessory. The open-input default now
  selects Speaker and passes 5.99 seconds at 440/880 Hz, along with boot and
  agent checks (`/tmp/it-regress-open-dock-native.log`). Snapshot audio restore
  remains independently blocked by live GL state.

### Expanded default tier accepted

The combined default run passed boot, full-volume fsck, clean-reboot persistence,
Harness installation and foreground launch, GLES color/hold checks, the guest
agent and 5.99 seconds of 440/880 Hz stereo audio. Both shutdowns were guest
confirmed. Evidence: `/tmp/it-harness-full-default.log`.

New ASan/UBSan tests decode production multitouch frames (empty, one-to-five
contacts and sparse slots), both guest versions' SDIO event headers, and actual
tcp_usb framing under fragmented reads/writes. The USB test replays five saved
real-device AFC file vectors plus synthetic odd-sized transfers and NAK replies;
it validates transport bytes, not a guest AFC server implementation.

### Attitude model

The accelerometer now has one mounted gravity calculation for pitch, roll and
upright/flat poses. Machine properties accept startup and live values; discrete
orientation commands share the same calculation, and raw axis overrides remain
available. The macOS bridge exposes an attitude call. Snapshot v2 preserves the
angles/pose, validates bounds, and restores a resting sensor after a transient
shake; v1 snapshots recover attitude metadata from their orientation.

Sanitizer checks cover cardinal and combined angles, one-g magnitude, invalid
inputs and snapshot metadata. Native paused-QEMU checks cover startup arguments,
live QOM updates, mounting signs and large raw-axis clamping:
`/tmp/it-attitude-qmp-v2.log`. Light Touch inputs and sensor sampling remain next.

### Light Touch motion inputs and sampled sensor

Light Touch now has Upright/Flat poses, two-axis off-panel scroll tilt, Option-arrow
keyboard tilt at three speeds, and Option-Space shake. Focus loss, sleep and key
release return to rest; focused production-method tests cover those transitions,
Live Text and guest-key isolation. Release build passes. Commits: Light Touch
`60b06ef`, macOS bridge export `c9b118aea1`.

The sensor samples on virtual-time boundaries (automatic 100/400 Hz or an explicit
1..400 Hz override), adds bounded one-count noise, and applies a 200 ms three-axis
shake. Native Harness acceptance passed callbacks and guest-confirmed shutdown
(`/tmp/it-accel-guest-native-v4.log`); its requested 100 ms UIKit interval measured
about 100.7 ms. Sanitizers cover rates, saturation, noise, shake and snapshot v3.
QOM axis readback reports requested raw values, while I2C exposes sampled values.

The guest revealed that 7E18 UIKit preserves raw X/Y signs and inverts Z, unlike
an earlier assumption in the plan. The shared model currently retains the plan's
raw-axis convention; physical face-up/steering acceptance remains open. See
`docs/accelerometer-controls.md`. Controller inputs and the attitude indicator
remain unfinished; the optional phone companion has not been built.

### App regression controls use the agent

Default app launch, lock/foreground queries, Harness reset and GLES diagnostics
now use the guest agent when available; legacy fixtures retain the explicit SSH
launcher, and shim staging still requests it. SpringBoard restart uses the same
agent path with bounded read-only readiness retries. Submitted mutations never
fall back to SSH. The launcher now prefers the current native build and honors
`QEMU`; interrupted runs stop their owned child processes.

The full eight-check default tier plus respring passed with `guest_ssh` replaced
by a function that raises on every call: `/tmp/it-agent-control-native-v3.log`.
This includes both guest-confirmed shutdowns, reboot persistence, full-volume
fsck, GLES and 5.99 seconds of 440/880 Hz audio. Focused failure/fallback tests
also pass. The updated, signed app is `build-native14/Light Touch-motion.app`.

### Refreshed default image

`nand-agent-v3` is a separate copy of the agent candidate, refreshed with current
MBX, launch helpers, command/typing agents and all five sound defaults. Baking
now sets the same Core Animation environment as the app's upgrade path. A new
HFS-catalog check validates exact helper bytes, guest ownership/modes, launch
configuration and preferences (`test_baked_components.py`).

The candidate passed the complete default tier plus respring with SSH disabled,
including accelerated graphics, 6.36 seconds of stereo audio, both PMU-confirmed
shutdowns, persistence and full-volume fsck: `/tmp/it-nand-agent-v3-native.log`.
The packaging script prefers v3 when available; an explicit LTM_NAND still wins.
Existing devices retain their selected base and overlay via DeviceStateStorage;
the refreshed base is for new devices or an explicitly requested factory reset.

### Native music-library import foundation

Track D's proposed legacy iTunesDB writer is superseded by measured 7E18
behavior: Music uses SQLite, and its native MusicLibrary import service adds
staged songs while maintaining its own indexes, locations and Purchased list.
`contrib/it-media/itmedia` validates metadata/paths/firmware, runs as mobile,
serializes imports and reconciles an already-imported location read-only.
The identity query avoids private collation indexes created by Music.

`tests/ipod/test_media_guest.py` passes two generated AAC/MP3 imports, duplicate
recovery, malformed-input rejection, Harness MediaPlayer count, Music Songs and
playback screenshots, 12.19 seconds of 440/880 Hz audio, cold persistence and a
second duplicate check. Both shutdowns are guest-confirmed. Evidence:
`/tmp/it-media-guest-native-v4.log`. Music's MIG service must be running before
the third-party MediaPlayer query; an unavailable response is reported explicitly.

Light Touch import/drop/progress integration, photos, artwork, additional
playlists and video-library import remain unfinished. The helper preserves
staged audio on ambiguous failure; retries must reuse the same staged location.

### Light Touch music import

File > Sync Media and audio-file drops now prepare an immutable copy, read
AVFoundation metadata, validate supported codecs/rates and queue AFC uploads
with app installations. Progress uses the existing inspector rows; cancellation
ends at the library commit boundary. The guest helper is included in packaging.

Release build and focused preflight/upload checks pass. The native app-side
test compiles the actual Swift preflight, DeviceServices, IMobileDevice and
DeviceTools music methods; real AFC preserves the exact audio bytes, and the
guest receives quoted/Unicode metadata unchanged apart from canonical Unicode
normalization. Repeated commit of the same staged location leaves one song.
Music's Songs screenshot and PMU-confirmed shutdown pass:
`/tmp/ltm-media-native-v2.log`. The test uses a local adapter to the isolated
guest agent; it does not claim interactive AppKit picker/drop acceptance.

At that checkpoint Photos, raw AAC transcoding, artwork, playlists, content
deduplication across separate jobs and recovery UI remained open. Photos is
implemented and accepted below; the other media extensions remain open.

### Native Saved Photos foundation

`itphoto` uses UIImageWriteToSavedPhotosAlbum after checking baseline JPEG
dimensions/size. Its persistent pending/done receipt distinguishes a confirmed
save from an uncertain outcome and prevents automatic duplicate replay. iOS
creates the DCIM original, album poster and BTH/THM thumbnail files. Confirmed
saves remove only the redundant staged copy.

Native validation passes image colors/dimensions, thumbnail files, duplicate
and uncertain receipts, malformed input, cold persistence and both shutdowns:
`/tmp/it-photo-guest-native-v2.log`. This run matches Light Touch's enabled media
hardware. The album poster and full-size photo render, but the grid thumbnail
is blank. That issue remains open; the test does not claim grid rendering passes.
Host photo preparation and UI integration are in progress, not yet packaged.

### Photos RGB555 thumbnail rendering

The blank grid was an unsupported L555 IOSurface, not missing thumbnail files.
The bridge now imports little-endian RGB555 with opaque alpha and writes FBO
results back as RGB555 while preserving guest row padding. Native CGL sanitizer
checks cover colors, bit-15 independence, writeback and sampled-memory refresh.
An isolated 7E18 Photos run shows the red/green/blue thumbnail and white circle:
`/tmp/it-photo-grid-rgb555.log`. The photo regression now asserts grid colors.

### Light Touch Photos acceptance and refreshed package

Photo preparation (orientation, size bounds, JPEG conversion, white alpha
background and cancellation), real AFC upload, native save and duplicate receipt
checks pass: `/tmp/ltm-photo-native-v2.log`. A separate refreshed default NAND v4
passes component ownership/content validation and the full native Photos test,
including visible grid colors and cold persistence: `/tmp/it-photo-guest-v4.log`.
The Light Touch photo integration is committed as c5e40de. The separate
`build-native14/Light Touch-photos.app` includes current emulator/guest components
and NAND v4; Release build, deep strict signature and macOS 14 library closure
checks pass. Existing device state remains preserved by the normal upgrade path.

### Light Touch battery controls

Device > Battery exposes target capacity with a slider, percentage, Empty/Full
labels and a capacity indicator, plus automatic/forced charging modes. Settings
persist to boot arguments; unsupported older dylibs disable the control. The
runtime bridge validates bounds/readiness and queues both changes on QEMU's
thread. ASan/UBSan bridge and PMU ADC regressions pass. The actual AppKit editor
was rendered at 5/15/80 percent and inspected for colors and unclipped labels.
Guest capacity filtering remains intact; the UI explains the delayed estimate.
Integration into a wider device-status pane remains
open. Earlier native calibration/charging evidence still covers the underlying
PMU properties; no new native guest acceptance is claimed for this UI wiring.

### Refreshed default-image regression

NAND v4 passes boot, full-volume read-only fsck, clean-shutdown persistence,
GLES color rendering, stereo 440/880 Hz audio, binary guest-agent transfers and
SpringBoard restart: `/tmp/ltm-v4-full-regression.log`. That command had an
incorrect IPA path and skipped install/launch; both checks were then run with
the actual built Harness IPA and passed: `/tmp/ltm-v4-app-regression.log`.
The native harness now prefers v4, with v3/v2/legacy fallbacks. Media tests use
v4 too. The battery-inclusive package `build-native14/Light Touch-battery.app`
passes Release build, strict deep signature and macOS 14 dependency checks.

### CLCD interrupt enable and pending status

The 7E18 frame handler disables source bit 0 at register 0x08 when idle. The
model ignored this register and treated an acknowledgement at 0x0c as permission
to keep raising interrupts. It now models enable, pending and W1C independently,
including migration compatibility. See `docs/ipod-clcd-irqs.md` for driver
addresses and the hardware contract. Sanitizer IRQ and LCD compositor tests pass.

The earlier 120-second untethered lock/wake run logged 787 unexpected CLCD
interrupts; the corrected run logs zero. Locked/60-second/95-second screenshots
are black, Home restores the lock screen, the guest agent answers, and shutdown
is guest-confirmed: `/tmp/it-idle-wake-clcd-fixed.log`. This proves display
blanking and wake after idle, not every deep-suspend or automatic-lock path.

All ten native checks pass with the corrected model: boot, full-volume fsck,
persistence, app install/launch, GLES colors, binary guest agent, stereo audio,
SpringBoard restart and serial console: `/tmp/it-clcd-full-regression.log`.


### CLCD package follow-through

The repeatable `test_sleep_guest.py` passes explicit black-panel, Home-wake,
no-reset, agent-recovery, no-unexpected-interrupt and clean-shutdown assertions:
`/tmp/it-sleep-guest-final.log`. Photos import, visible grid colors and cold
persistence also pass after the IRQ change: `/tmp/it-photos-clcd-regression.log`.

### Raw AAC import

Light Touch now accepts raw AAC and streams it through macOS audio codecs into
an AAC-LC M4A before upload, without an external converter. Reads use the decoded
frame count because AVAudioFile reports eofErr on a read beyond an ADTS file's
end. Conversion bounds duration/output size and checks cancellation between
buffers. Existing MP3/M4A/WAV copies remain byte-identical.

Actual Swift preparation passes raw AAC plus the existing audio fixtures. Native
AFC/import/duplicate reconciliation passes and Music plays 6.21 seconds with
440/880 Hz channel peaks: `/tmp/ltm-aac-native-v3.log`. Release build passes.
The one-song playback test uses the first row, without the Shuffle row present
in the earlier two-song test. Music leaves the foreground during volume setup;
no CrashReporter files were present, and relaunch/playback passes. That separate
foreground transition remains unexplained and is not claimed fixed.

### Automatic lock acceptance

`test_sleep_guest.py --automatic` sets one-minute Auto-Lock through Settings in
an isolated guest, verifies a black panel after 95 seconds, wakes with Power,
checks agent recovery without reset or unexpected CLCD interrupts, and confirms
PMU shutdown. `/tmp/it-auto-sleep-guest.log` passes. The default NAND retains
its existing Never setting.

### Automatic battery drain and USB control

The PMU samples fractional percent-per-minute drain against virtual time, with
zero disabling it. Pause and USB charging freeze drain. Native acceptance
`test_battery_guest.py` passes target reduction, guest voltage 3917 → 3906 mV,
pause freeze, USB charging freeze and guest-confirmed shutdown in 114 seconds:
`/tmp/it-battery-drain-native-v5.log`. The guest's filtered capacity initially
moves only 60 → 59; it is deliberately not overwritten by the host target.

7E18 can defer voltage sampling with USB connected but charging forced off.
The native test uses normal unplugged discharge. Light Touch therefore exposes
virtual USB connection alongside level, charging and drain, with transfer guards
and automatic reconnection before restart/Power On. Agent-based sync/shutdown
remain available while USB is disconnected. Core/bridge sanitizer checks and
the Release build pass; the production editor and controller checks cover
fractional input, invalid rates, transfer guard and reconnect behavior.

### Guest audio in screen recordings

Light Touch records the QEMU output mixer directly as stereo AAC alongside H.264,
without microphone capture. Generation-scoped bounded packets share a monotonic
clock with video. Explicit zero PCM preserves idle and VM-pause gaps; timestamps
alone let AAC collapse those gaps. Stop drains queued samples before finalizing.

`test_recording_audio.py` passes sanitizers for packet boundaries, overflow,
clock gaps, stale generations, double cleanup and post-stop draining. Light Touch
`tests/check-recording-audio.py` verifies the production AAC/video writer.
`tests/check-media-native.py --aac --recording` embeds the actual QEMU dylib and
production movie writer, imports AAC through native Music, verifies 440/880 Hz
stereo before/after a two-second silent VM pause, and confirms guest shutdown:
`/tmp/ltm-recording-native-final.log`. Release build passes.

Music's intermittent first Songs-tab exit is reproduced before volume input;
volume buttons are not the cause. No CrashReporter output accompanies it. The
playback acceptance relaunches Music and does not claim this exit is fixed.

### Settings, menus and controller input

Light Touch now has a fixed-size Settings window for rotation, keyboard tilt
speed, catalog URL, controller stick response and A-button coordinates. Resume
on launch remains removed. Device menus expose the existing volume, pause,
snapshot and power-off actions; rotation yields to text editors and saved-state
discard confirms first. Dock actions reuse the same window controller.

Controller input uses Apple's GameController framework on the existing display
tick: left stick maps through a 0.1 deadzone to ±45 degrees, A holds a configured
screen point, Menu sends Home and Options shakes. Native controller snapshots
verify button edges, focus/sleep suppression, disconnection release and response
curves; production touch tests verify release before mouse takeover. Keyboard
motion, Settings and Release checks pass. Physical-controller/game acceptance
is still pending. API reference: https://developer.apple.com/documentation/gamecontroller/gccontroller

### Music's first-launch exit

The guest trace caught `SyncHelper._delayedTerminate` calling
`UIApplication.terminateWithSuccess`, status 0. The helper now completes native
ITSync post-processing synchronously before reporting import success, so Music
need not perform unfinished import work on its first launch. Three fresh-overlay
native Swift/AFC/import/playback tests pass without the former recovery relaunch,
including Songs, volume input, stereo playback and confirmed shutdown:
`/tmp/ltm-music-postprocess-native.log` and `-v2.log`/`-v3.log`.

### Capture and controls package acceptance

The native recording test also passes after the Music lifecycle fix:
`/tmp/ltm-music-recording-postprocess.log`. A 40-point attitude horizon now shows
pitch/roll during tilt and provides an accessible Level action; native rendering
and action checks pass. Help is a bundled, searchable native text window.

### Content-based music import reconciliation

New song imports derive their IDs from prepared content. Existing AFC files are
compared byte-for-byte before reuse; a mismatch is rejected without changing the
library file. New media uploads publish only after close/rename, so interrupted
writes never truncate a canonical library file. Repeated raw AAC conversion now
zeros only generated mvhd/tkhd/mdhd creation/modification timestamps; the audio
payload is unchanged. Bounded container, conversion identity, native repeated
import, mismatch rejection and stereo playback checks pass, as does native
Photos after the shared upload change. Evidence: `/tmp/ltm-media-dedup-native-v2.log`,
`/tmp/ltm-photo-atomic-native.log`, `/tmp/ltm-media-identity-check.log`.

Existing random-ID song imports are not retroactively indexed. Photo content
identity remains pending because its receipts need to distinguish a saved photo
from one subsequently deleted in Photos. Photo retry receipts retain their
previous behavior; new photo uploads gain atomic publication only.

### Recovery and Apps menu follow-through

Light Touch session-tags staging files so a delayed startup sweep cannot remove
new uploads. It also removes only recognized abandoned atomic media temporary
files; canonical media, receipts and unknown files remain. Native Photos/AFC
acceptance seeded old and current uploads, swept them, verified preservation,
then checked the single saved photo and guest-confirmed shutdown
(`/tmp/ltm-photo-cleanup-native.log`). The Apps menu shares the inspector's
existing actions, using selected rows while context menus use clicked rows.
Native menu checks cover selection, batches, cancellation and empty lists.

### Retired-service fast failure

Direct proxy mode returns HTTP 410 for exactly `api.openfeint.com` and
`gdata.youtube.com`, including CONNECT, before contacting an origin. Dated,
disabled and upstream modes bypass the policy. Host sanitizer tests cover
authority parsing, lookalike domains and archive access; TLS regressions pass.
Native NSURLConnection returned prompt HTTP 410 for both hosts after the normal
HTTP fixture passed (`/tmp/it-proxy-retired-native.log`). This is a failure path,
not service revival or a blanket advertising/analytics block.

### Native logs and first typed configuration option

Light Touch's File → Device Logs displays bounded, off-main-thread tails of the
current and rotated serial/kernel and USB logs. It supports Find, selection/copy,
pause, rotation and close/reopen; native window checks and the Release build pass.
App-event persistence and replacing informational alerts remain separate work.

The first configuration migration is `audio-hw=auto|on|off`, listed by QEMU's
machine help. Explicit options override `IT_AUDIO_HW`, including explicit auto;
legacy defaults remain and runtime topology changes are rejected. Sanitizer and
harness checks pass. Native boot/install/launch and 6.18 seconds of 440/880 Hz
audio passed with `audio-hw=on` overriding `IT_AUDIO_HW=0`
(`/tmp/it-audio-config-native.log`). See [configuration](configuration.md).
The unused app-side `IT_IMG3_SIG_ASIS` setting was removed, and boot-parity checks
now understand the current verbose/kernel-console expression and reject obsolete
switches. The rest of the configuration migration is pending.

### App-ledger collection

`regress.py --ledger DIRECTORY` now runs each IPA sequentially on a fresh guest,
retains 5/20-second screenshots and 30-second launch verification, records the
file SHA-256 and bundle ID, and atomically updates Markdown/JSON progress.
Skipped/failed checks and changed input files cannot pass. Rendering, audio,
input and networking stay unreviewed. A native Harness run passed; its initial
menu screenshots were reviewed and retained in [the ledger](app-ledger.md).
Broader game coverage and Legacy Store verdict presentation remain pending.

### Stock-service protocol research

A disposable 7E18 Weather guest accepted local synthetic XML, persisted both
replacement city names, displayed the six-day forecast, and shut down cleanly
(`/tmp/it-weather-protocol-native-v2.log`). The runnable protocol check retains
its screenshot and sanitized requests. [Protocol notes](stock-service-protocol.md)
record the strict six-forecast shape and Stocks' captured request types.
Live provider integration and native city-search acceptance remain pending;
no fabricated weather or quotes are served by the production proxy.

Native city search now displays and saves an opaque replacement Weather ID.
The consolidated `test_stock_services_guest.py --stocks` check verifies all ten
persisted synthetic quotes, including price/change and market-cap conversion.
Its 60-point synthetic chart rendered the expected sawtooth; both guests shut
down cleanly (`/tmp/it-weather-search-native.log`, `/tmp/it-stocks-protocol-native.log`).
This establishes protocol acceptance, not live Weather/Stocks revival.

### Built-in live Weather

Direct HTTP proxy mode now serves the stock Weather gateway from Open-Meteo:
city search, the two default cities, new opaque IDs, current conditions and six
forecast days. Native Foundation handles XML/JSON; requests and responses are
bounded, host TLS is verified, and invalid/unknown data fails without invented
weather. Existing unknown Yahoo IDs need removal/re-addition. The Weather link
and Light Touch Help attribute the noncommercial free API. The moon icon uses a
documented mean-cycle approximation; English search names and unavailable polar
sunrise/sunset remain limitations. Archive/off/upstream behavior is unchanged.

Sanitizer/parser/framing checks and HTTP/TLS regressions pass. A fresh native guest
rendered live Fahrenheit forecasts, searched/added Cupertino, switched to Celsius,
persisted all three six-day forecasts, and shut down cleanly
(`/tmp/it-weather-live-native-final-v2.log`). Stock Weather converts cached
Fahrenheit data for Celsius display. Release app build and native Help checks
pass. Stocks remains at synthetic protocol verification, without a live provider.

### Package and tooling follow-through

`build-native14/Light Touch-latest.app` now points to `Light Touch-45c117d.app`,
with the live Weather helper and Help attribution. Deep signature verification
and macOS 14 dependency-closure checks passed (`/tmp/ltm-weather-package-check.log`).

Screenshot normalization, recording and the regression harness now share one
bounded PPM reader. Truncated headers/comments fail instead of looping forever;
malformed-input checks, existing regression checks and retained native frames pass.
`bootshot.py` uses that reader, fresh overlays and owned-QEMU identity checks. It
stops its disposable guest by default and requires `--keep-running` to retain it.
A native eight-second run captured seven early boot frames and stopped; this is
capture cleanup, not a clean-shutdown/storage acceptance test.

AppSync documentation now describes the four 7E18 sites and entitlement
preservation. The two 5F138-only patch tools and old 500 MB pruning recipe are
marked historical. Networking notes distinguish the old resolver investigation
from current 7E18 behavior; capabilities now link agent/configuration interfaces.

### Native orientation polling (2026-09-06)

Light Touch now uses the guest agent for orientation after media preparation;
only images with no agent retain the streamed SSH helper. Polling preserves
manual rotation, rejects stale boot callbacks, and pauses during sleep/install.
The 7E18 MIG request is firmware-checked and bounds send/receive to 250 ms each,
with a private reply port cleaned up on success and failure. App-name polling
keeps its existing cancellable helper: the native SBS API is also unbounded.

`test_agent_orientation.py` passes ASan/UBSan ABI, timeout, cleanup, firmware and
invalid-result checks. `test_agent_guest.py --orientation` passes native
landscape Harness → Home → stopped SpringBoard (ETIMEDOUT, then ping succeeds)
→ respring → orientation, followed by guest-confirmed shutdown in 87 seconds
(`/tmp/it-agent-orientation-native-final.log`). Light Touch's
`tests/check-agent-transport.py` passes concurrent reply routing, binary exec,
invalid orientation, absent/stale agent and cancellation; Release build passes.

### App-event logs and persistent failure status (2026-09-06)

Light Touch `dacd1af` records existing app events through a serial utility queue,
with UTF-8 byte-bounded entries (32 KB), private files and two 1 MB generations.
File → Device Logs and Export Diagnostics include app.log/app.log.1; export
waits for queued events. Disk-write failure stops further file writes rather
than spinning or growing logs. Unified logging still receives the events.

Preparation, explicit state-save, erase, restore and Power Off failures use a
native titlebar status bar with Show Logs and Dismiss. Notices survive relaunch;
a successful retry clears only its own operation. Storage failures override
other notices and cannot be dismissed while the storage latch is set. Restart,
Erase and Discard confirmations remain decisions. This is not a complete device
status/controls pane or a general alert rewrite.

`check-app-events.py` passes concurrent append, format/literal-percent handling,
UTF-8/combining-character bounds, permissions, rotation, write failures, persisted
notice state, resolution and storage priority. `check-log-view.py` passes native
visibility/layout at 360/640 points, Show Logs/Dismiss behavior, selection/pause
and close/reopen. Screenshot capture was unavailable; these are native layout
and control checks, not a claimed visual screenshot review. Release and Help
checks pass. Full catalog/UI checks pass, after repairing stale AFC/rotation
fixtures and replacing a reproduced timer-order race in the queue test with
explicit readiness gates (`cd6309d`). Evidence: `/tmp/ltm-app-events-check-v3.log`,
`/tmp/ltm-device-notice-ui-final.log`, `/tmp/ltm-event-catalog-checks-v6.log`,
`/tmp/ltm-device-notice-build-v3.log`.

### Shared firmware constants and safe kernel patches (2026-09-06)

Exact verified 5F138/7E18 banners now select a shared firmware table. A legacy
MBX register read previously wrote 2.1.1 BCM4325/USB addresses even when the
kernel was unrecognized; those writes and the legacy clock patch are now
5F138-only. 7E18 keeps the modeled PMU RTC path. AMFI default addresses require
a detected mapped kernel; unknown research builds require all three explicit
address/slide overrides. FMSS's existing 5F138 boot-argument buffer is centralized.

Production detector/patch tests pass ASan/UBSan, including partial/ambiguous
banners, early retry, cache/reset, zero writes to unknown/7E18 through MBX,
legacy 5F138 writes and AMFI override guards. Early boot/NVRAM/audio configuration
checks pass. Native 7E18 with AMFI enabled reaches Settings, survives a legacy
MBX read with all five clock/driver regions unchanged, answers the agent, and
confirms guest shutdown in 87.9 seconds (`/tmp/it-firmware-native.log`). Its
retained clock-function bytes confirm no legacy clock patch was present before
the read. Native 5F138 boot, the firmware-selection property and other behavior
properties remain pending; see `configuration.md`.


### Legacy clock trampoline removal (2026-09-06)

A fresh 5F138 boot exposed an undefined-instruction exception with the link
register inside the MBX-injected clock entry. The injected MRC is a Thumb-2
instruction; ARM1176 supports Thumb-1. Both kernels already have a modeled PMU
RTC, so the trampoline and its unused profile addresses were removed. The
compatibility cp15 clock register remains for old research kernels.

The guarded-patch sanitizer test now asserts no write to the 5F138 clock entry.
Native 5F138 reaches the Home screen with its original RTC path
(`/tmp/it-firmware-5f138-rtc.log`, retained `frame-3.png` in
`/var/folders/tp/360v5_ln3lxg5x66gf0rqc540000gn/T/it-5f138-boot-a5ni3ud5`).
The earlier run stopped in the clock exception (`/tmp/it-firmware-5f138.log`).
The newer run later closed QMP around the untethered idle transition; that
separate behavior is not yet diagnosed or claimed stable.


### Legacy idle is not shutdown (2026-09-06)

A PMU trace establishes that the 5F138 idle exit was generated by the emulator:
register 0x10 changed from 0x7f to 0x3f, triggering the old shutdown heuristic.
Letting the guest continue shows regulator/wake-mask setup, final command
0x6f=0x80 and "System Sleep" / "pmu go hib". Register 0x10 is therefore not a
unique shutdown indicator. Removed that heuristic; the verified native standby
command 0x6f=0x90 remains independent of host arming.

The guest remains alive for 120 seconds through this transition
(`/tmp/it-firmware-5f138-nolatch.log`). Home wake did not resume its deeper
hibernation path, and the separate legacy power-off gesture did not complete;
neither capability is claimed fixed. The production PMU write-handler check
now rejects shutdown for the observed idle sequence. ADC/battery and guarded
firmware patch checks also pass.
Native 7E18 still passes firmware-memory preservation, agent operations, Settings
launch and guest-confirmed shutdown with QEMU exit 0 in 87.9 seconds
(`/tmp/it-pmu-no-latch-7e18.log`).
