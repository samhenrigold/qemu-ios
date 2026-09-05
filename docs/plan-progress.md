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
