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

Implemented: ping, exec (binary stdin and combined stdout/stderr), put (path and
final octal mode, atomic rename), get (whole regular file), getrange (`offset length path`, binary output), settime, launch,
frontmost (bundle id and localized name), lockstatus, kill (executable name),
halt (launchd shutdown request; the host must still await PMU confirmation).
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

The daemon waits 40 seconds before starting and corrects the guest wall clock
when drift exceeds two seconds. It touches receive-buffer pages before host
copies, because debug memory writes cannot fault in iOS demand-zero pages.

Tests: `test_agent_proto.py` and `test_agent_ops.py` exercise production C under
ASan/UBSan. `test_agent_guest.py` boots a fresh native overlay and verifies binary
transfers, root shell execution, clock correction and SpringBoard operations.
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
existing-device rollout remain subsequent steps.
