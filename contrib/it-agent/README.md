# Local guest agent

`it_agent` replaces `it_pbd`, retaining its explicit-UTI clipboard implementation.
It serves bounded host RPCs through the existing cp15 tunnel. It has no socket
listener. Build with `ARMV6_SDK=/path/to/iPhoneOS3.1.3.sdk bash build.sh`.

Install `/usr/local/bin/it_agent` (root:wheel 0755) and the launch job in
`/System/Library/LaunchDaemons` (root:wheel 0644). Remove/unload the old
`com.qemu.it-pbd` job so two clipboard daemons never compete. The bake script
performs the file changes; its documented ownership step is required.

Requests are `id operation arguments\n<binary body>` on the guest wire, with a
base64 body in QOM's `agent-request` string. `agent-result` returns
`id status\n<base64 body>`. `agent-cancel` accepts an id. `agent-status` is
absent/alive/stale.

`ping` answers `it_agent v4\nops <space-separated op list>\n` (v3 the same without
`cadebug` and `statusbar`, v2 also without `putpart`); a v1 agent answers only `it_agent v1\n` and returns -ENOSYS (-78) for
every op it lacks, so a host detects capabilities from the ping reply. Statuses are 0, a child's exit status,
or a negative errno.

| op | args | body | reply | notes |
|---|---|---|---|---|
| ping | | | version + op list | |
| spawn | | argv as NUL-terminated strings, argv[0] absolute | child's stdout+stderr, status = exit status (128+signal) | no shell; stdin is empty. v2 |
| sync | | | | sync(2). v2 |
| put | `path mode` (octal, last word) | file bytes | | mkstemp beside path, fchmod, fsync, rename (atomic); root-owned |
| putpart | `offset final mode path` | file bytes from offset | | a put over one request: chunks append to `path.it-agent-part` in order (offset 0 starts it, else it must equal the part's size, or -EINVAL); `final` 1 fchmods, fsyncs and renames it over path (atomic). Any failure discards the part. v3 |
| get | `path` | | file bytes | regular files up to 1 MiB, no symlinks; -ENOENT if absent |
| getrange | `offset length path` | | bytes | |
| chown | `uid gid path` | | | lchown(2); run after put for mobile-owned files. v2 |
| unlink | `path` | | | unlink(2); -ENOENT if absent. v2 |
| settime | `epoch` | | | |
| launch | `bundle-id` | | | stock SBSLaunchApplicationWithIdentifier, or 2.x SBLaunchApplication |
| frontmost | | | `bundle-id\nlocalized name\n` | `com.apple.springboard\nLock Screen\n` when locked |
| lockstatus | | | `locked=0/1 passcode=0/1\n` | |
| orientation | | | `0/90/180/-90\n` | 7E18 ABI only, else -ENOSYS |
| dlicon | `add <unique-id> [<bundle-id>]` or `cancel <unique-id>` | | | install placeholder (sbdlicon's SBS calls); -EAGAIN when SpringBoard declines. v2 |
| halt | | | | reboot2(halt); await the PMU shutdown on the host |
| cadebug | `mask value` | | flags now, `0x…\n` | Core Animation's debug colors: QuartzCore's `CARenderServerSetDebugFlags(0, mask, value)` (the render server's flags become `flags & ~mask \| value & mask`; 0x4 blended layers, 0x2 copied images, 0x4000 misaligned, 0x20000 offscreen, 0x1 flash updates, 3.1.3 to 7.1.2). v4 |
| statusbar | | UIKit's status bar override data, laid out by the host for the firmware | | `+[UIStatusBarServer postStatusBarOverrideData:]` (4.2 to 7.1.2); SpringBoard takes it only from a process with `com.apple.UIKit.status-bar-override-allow` (`it_agent-entitlements.xml`). Each post replaces the last; zeros clear it. v4 |
| type, backspace, uidump | | UTF-8 text (type) | | routed to it_typein in the foreground app |
| exec | shell command | stdin | stdout+stderr | **deprecated**: needs `/bin/sh`, which a no-shell image lacks; kept only for old hosts |

`kill` (v1) is gone: it shelled out to `killall`. Restart a daemon or SpringBoard
with `spawn /bin/launchctl stop <label>` (KeepAlive relaunches it). Upgrade the
agent itself with `put /usr/local/bin/it_agent 755` then `spawn /bin/launchctl
stop com.qemu.it-agent`: the reply is -ECONNRESET and the new binary claims the
channel about 11 s later.
Commands use a fixed guest PATH and C locale. Output is capped at 1 MiB,
requests at 256 KiB, chunks at 1024 bytes, and outstanding requests at 16.
Files of any size still move: `itqmp.agent` sends a put over one request as
`putpart` chunks (raising a clear error on a v2 agent, whose limit it would exceed)
and reads a get the agent refuses with -EFBIG back by `getrange`. The QOM setter
reports an over-limit request as such, not as a queue error.
Execution runs in a child process group with a roughly 60-second tick budget.
The daemon keeps polling and servicing clipboard during child execution.

Tokens correlate daemon sessions; they are **not a privilege boundary** against
other guest processes that can issue cp15 calls. Keep QMP local and never expose
this root command channel over the guest network. A ten-second missed heartbeat
allows a replacement daemon to claim the channel. An interrupted dispatched
request returns `-ECONNRESET`, never automatic replay. Host cancellation revokes
the old session; its next poll kills the child group. Cancellation cannot undo an
already executed command. Reset clears transient requests/results.

Command service starts immediately; only the clipboard waits 40 seconds for
UIKit readiness. The agent corrects the guest wall clock when drift exceeds two seconds. It touches receive-buffer pages before host
copies, because debug memory writes cannot fault in iOS demand-zero pages.

Tests: `tests/slice/ipod-agent-proto.c` and `ipod-agent-ops.c` exercise production C under
ASan/UBSan. `test_agent_guest.py` (retired; see git history at 5508b504b8) booted a fresh native overlay and verified binary
transfers, shell-free spawn/sync/chown/unlink/dlicon, clock correction and SpringBoard operations.
`it_typein.dylib` is inherited by SpringBoard-spawned UIKit apps. It receives
`type` (UTF-8 body), `backspace`, and `uidump` requests routed by the daemon to the
actual foreground PID. A separate per-request cookie and five-second deadline
reject stale UI replies. UI clients use the same bounded transfer code and run
all UIKit operations on the main run loop. There is no signal-handler override
or cross-process UIKit access. Bulk text uses the focused delegate's insertText:
method; physical keys use UIKeyboardImpl's one-key path. A non-consuming key
check avoids polling SpringBoardServices while idle.

Native acceptance covered Notes and an installed Harness UITextField
(the retired `test_agent_guest.py --typing`). Snapshot-load rekeying and
existing-device rollout are covered by native acceptance and the app’s idempotent
media upgrade. SBS focus queries run on a worker thread: synchronous queries on
SpringBoard’s own main thread deadlock its service. UIKit mutations stay on the
main run loop. SpringBoard uses route target zero for Spotlight and lock-screen
inspection; locked input is rejected and unfocused physical keys are discarded.

### Orientation polling

Light Touch uses the native `orientation` request after media preparation, so
current images do not need a persistent SSH/iproxy/itorient session. It reports
SpringBoard's frontmost-app orientation, not accelerometer posture. The original
7E18 `SBGetUIOrientation` stub has no timeout; this operation checks its first
8 bytes, uses its verified request ID `0x1e8496` and 40-byte reply, and bounds
send/receive waits to 250 ms each. A private reply port is destroyed on every
path, including timeout. Other firmware stubs fail with ENOSYS rather than
assuming this ABI. The host retries without changing orientation on failure.

`tests/slice/ipod-agent-orientation.c` is the bounded ABI check; the retired
`test_agent_guest.py --orientation` exercised a
landscape Harness, Home, a stopped SpringBoard and respring on disposable media.

The exported armv6 helpers use the existing legacy linker mode: classic dyld
metadata, the reserved r9 thread pointer and old-libSystem startup. The iPad's
armv7 build remains separate. `it_typein.dylib` is ad-hoc signed in the build
recipe: 2.x and 3.0 kill SpringBoard when its injected executable page is
unsigned, even though later kernels accept the configured enforcement flags.
See [legacy helper qualification](../../docs/research/legacy-helper-abi.md) for
actual firmware tests and the unqualified operations.
