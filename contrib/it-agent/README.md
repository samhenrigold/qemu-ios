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
absent/alive/stale. The QMP helper exposes `agent(q, op, args, body)` and the CLI
`python3 imgtools/itqmp.py PORT agent ping`.

`ping` answers `it_agent v2\nops <space-separated op list>\n`; a v1 agent answers
only `it_agent v1\n` and returns -ENOSYS (-78) for every op it lacks, so a host
detects capabilities from the ping reply. Statuses are 0, a child's exit status,
or a negative errno.

| op | args | body | reply | notes |
|---|---|---|---|---|
| ping | | | version + op list | |
| spawn | | argv as NUL-terminated strings, argv[0] absolute | child's stdout+stderr, status = exit status (128+signal) | no shell; stdin is empty. v2 |
| sync | | | | sync(2). v2 |
| put | `path mode` (octal, last word) | file bytes | | mkstemp beside path, fchmod, fsync, rename (atomic); root-owned |
| get | `path` | | file bytes | regular files up to 1 MiB, no symlinks; -ENOENT if absent |
| getrange | `offset length path` | | bytes | |
| chown | `uid gid path` | | | lchown(2); run after put for mobile-owned files. v2 |
| unlink | `path` | | | unlink(2); -ENOENT if absent. v2 |
| settime | `epoch` | | | |
| launch | `bundle-id` | | | SBSLaunchApplicationWithIdentifier |
| frontmost | | | `bundle-id\nlocalized name\n` | `com.apple.springboard\nLock Screen\n` when locked |
| lockstatus | | | `locked=0/1 passcode=0/1\n` | |
| orientation | | | `0/90/180/-90\n` | 7E18 ABI only, else -ENOSYS |
| dlicon | `add <unique-id> [<bundle-id>]` or `cancel <unique-id>` | | | install placeholder (sbdlicon's SBS calls); -EAGAIN when SpringBoard declines. v2 |
| halt | | | | reboot2(halt); await the PMU shutdown on the host |
| type, backspace, uidump | | UTF-8 text (type) | | routed to it_typein in the foreground app |
| exec | shell command | stdin | stdout+stderr | **deprecated**: needs `/bin/sh`, which a no-shell image lacks; kept only for old hosts |

`kill` (v1) is gone: it shelled out to `killall`. Restart a daemon or SpringBoard
with `spawn /bin/launchctl stop <label>` (KeepAlive relaunches it). Upgrade the
agent itself with `put /usr/local/bin/it_agent 755` then `spawn /bin/launchctl
stop com.qemu.it-agent`: the reply is -ECONNRESET and the new binary claims the
channel about 11 s later.
Commands use a fixed guest PATH and C locale. Output is capped at 1 MiB,
requests at 256 KiB, chunks at 1024 bytes, and outstanding requests at 16.
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

Tests: `test_agent_proto.py` and `test_agent_ops.py` exercise production C under
ASan/UBSan. `test_agent_guest.py` boots a fresh native overlay and verifies binary
transfers, shell-free spawn/sync/chown/unlink/dlicon, clock correction and SpringBoard operations.
`it_typein.dylib` is inherited by SpringBoard-spawned UIKit apps. It receives
`type` (UTF-8 body), `backspace`, and `uidump` requests routed by the daemon to the
actual foreground PID. A separate per-request cookie and five-second deadline
reject stale UI replies. UI clients use the same bounded transfer code and run
all UIKit operations on the main run loop. There is no signal-handler override
or cross-process UIKit access. Bulk text uses the focused delegate's insertText:
method; physical keys use UIKeyboardImpl's one-key path. A non-consuming key
check avoids polling SpringBoardServices while idle.

Native acceptance covers Notes and an installed Harness UITextField:
`python3 tests/ipod/test_agent_guest.py --typing`. Snapshot-load rekeying and
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

Run `python3 tests/ipod/test_agent_orientation.py` for the bounded ABI check;
`test_agent_guest.py --orientation --base-nand .../nand-agent-v4` exercises a
landscape Harness, Home, a stopped SpringBoard and respring on disposable media.
