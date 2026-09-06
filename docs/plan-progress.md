# Emulator and Light Touch plan progress

The workstream follows the supplied hardware-fidelity plan, with reproducible
storage corruption promoted ahead of new peripherals. Each completed item needs
both a focused regression and relevant guest evidence. Bluetooth peers and
TV-out were explicitly deferred by that plan.

| Track | Status | Acceptance evidence still needed |
| --- | --- | --- |
| Generated NAND integrity and restart stability | Complete for the reproduced failures: free-pool bounds, FMSS completion, VIC fixes and paced TV-out IRQs | Twelve alternating Coldplay/Spore install/respring cycles; unchanged system file; guest shutdown; cold boot with both apps; full-volume read-only fsck passed |
| PMU ADC and masked event IRQ | Complete | Ten-bit results, settling vs conversion, mask/read-to-clear and GPIO tests pass; real 7E18 boot, three lock/wake cycles and native shutdown pass |
| Battery controls | Core implemented and guest verified; Light Touch integration pending | 20/60 percent cold calibration, full-voltage estimate, runtime off/on/auto and native shutdown pass; preserve guest filtering delay |
| Headset/Mikey detection | Deferred at user request (2026-09-05) | Plug/unplug and headset button traces; correct guest routing |
| Microphone/I2S RX | Deferred at user request (2026-09-05) | Deterministic input tone captured by the guest, then host microphone recording |
| Native idle sleep/wake | Pending | Auto-lock, actual display/CPU sleep, power/Home wake without brightness overrides |
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
  a 68-second UI recording is playable. Guest audio recording remains pending.
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

Photos, raw AAC transcoding, artwork, playlists, content deduplication across
separate import jobs and recovery UI for uncertain imports remain open.

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
