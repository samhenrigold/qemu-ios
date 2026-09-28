# iPod touch 2G guest services without SSH: inventory, feasibility, plan

Goal: the emulated iPod (iPhone OS 3.1.3 7E18 today, 4.2.1 8C148 and 2.x later) should follow the
iPad's vanilla-guest rule. That means stock lockdown/USB services first, then hardware models, then
one small hypercall agent. It should not rely on a shell, sshd or third-party guest binaries. The
IPSW pipeline then no longer needs the freeze (shell), OpenSSH and OpenSSL tarballs from
Legacy-iOS-Kit (`imgtools/ipod2g-shell.txt`).

Sources: LightTouchMac-multidevice @ 002db9f (line numbers below), qemu-ios `ipad1` @ e06c757ebb, the
7E18 rootfs (`qemu-ios-files/ios3/rootfs313.dmg`), and the 8C148 rootfs and kernelcache decrypted by
the IPSW pipeline (`qemu-ios-files/ipod-ipsw/cache/b9ef…/`; 7E18 kernelcache in `cache/5f4f…/`).
Probes: `tests/ipod/probe_guest_services.py` (this branch) on throwaway overlays of `nand-current.new`.

## 0. Where the shell is used today

There are two transports behind `DeviceTools.guestRun` (DeviceTools.swift:861):

- the agent's `exec` op, which is `posix_spawn("/bin/sh", "-c", command)` in
  contrib/it-agent/agent-ops.h. It is used whenever `qemu_ios_agent_status() == 1`, and it still
  needs freeze's `/bin/sh`, `cat`, `chmod`, `chown`, `mv`, `rm`, `killall` and `sync`;
- iproxy plus `ssh root@127.0.0.1` with the password `alpine` through SSH_ASKPASS
  (DeviceTools.swift:873-929). This is the fallback, the only path for `expecting:` commands (the
  halt), and it is forced for `usingAgent: false`.

The stock 7E18 rootfs has no shell. `/bin` holds only `launchctl`, and `/usr/bin` holds only
`IQAgent`, `powerlog` and `simulatecrash`. Every `exec` therefore depends on freeze today, even when
no SSH is involved. None of our own guest helpers link anything but `/usr/lib/libSystem.B.dylib`
(`otool -L`), and they `dlopen` only stock frameworks. Only the shell and sshd come from the tarballs.

## 1. Inventory

Freq: B = every boot, P = polling loop, U = user action, I = per install or import, F = fallback only.

### 1a. App call sites (LightTouchMac-multidevice)

| # | file:line | exact command (guest side) | feature | freq | failure modes | replacement |
|---|---|---|---|---|---|---|
| 1 | DeviceTools.swift:115 | `cat > /tmp/ltm-itmedia-<id> && chmod 755 … && chown 501:501 /var/mobile/Media/LightTouch /var/mobile/Media/LightTouch/<id>` (stdin = itmedia) | Music/Videos import: stage the helper | I | 30 s guestRun cap; >250 KB body refused by the agent; no shell | agent `put … 755` (exists) + `chown` (new) |
| 2 | DeviceTools.swift:118 | `cat > /tmp/ltm-media-<id>.plist && chmod 644 … && <exe> <plist> <id>; result=$?; rm -f …; exit $result` | Music/Videos import: commit to MusicLibrary | I | the stdout must end in `imported\n`, otherwise "media retained"; timeout → unknown outcome | `put` plist + `spawn [/tmp/ltm-itmedia, plist, id]` + `unlink` |
| 3 | DeviceTools.swift:153 | `cat > /tmp/ltm-itphoto-<id> && chmod 755 && chown 501:501 … && <exe> <id>; …; rm -f <exe>` | Photos import commit | I | as #2 | `put` + `chown` + `spawn` + `unlink` |
| 4 | DeviceTools.swift:225-226, 847 (also :836 from the catalog) | `/usr/local/bin/sbdlicon add\|cancel '<qemu-install-id>'` | "Waiting…" placeholder on the home screen during installs | I (2-3 per install) | fire-and-forget; a lost cancel leaves the icon until respring (saveIconState:NO) | new agent op `dlicon add\|cancel <id>` (port sbdlicon's SBAddDownloadingIconForDisplayIdentifier into agent-sbs.h) |
| 5 | DeviceTools.swift:498 | `killall SpringBoard` | Device ▸ Restart SpringBoard | U | needs freeze `killall` | **`spawn [/bin/launchctl, stop, com.apple.SpringBoard]`**: prototyped and proven, §3c |
| 6 | DeviceTools.swift:535 | GuestFileSnapshot.swift:12, `set -e; for file in …7 paths…; do printf '%s' <marker>; if [ -f "$file" ]; then cat "$file"; fi; done; …` | Media preparation: read the SpringBoard prefs/job, the GL engine, the agent, typein, the agent job, the legacy it-pbd job | B | 2 MiB per file; the 1 MiB agent response cap applies to the whole concatenation | loop of agent `get <path>` (exists; `-ENOENT` = absent) |
| 7 | DeviceTools.swift:553 | `cat > <dst>.ltm-new && chmod <mode> <dst>.ltm-new && mv -f <dst>.ltm-new <dst>` for it_agent, it_typein.dylib, com.qemu.it-agent.plist | Upgrade the agent in place | B (only when changed) | 250 KB body cap | `put <dst> <mode>` (exists; mkstemp + fsync + rename, already atomic) |
| 8 | DeviceTools.swift:561 | same, for the MBXGLEngine shim | GL engine upgrade | B (when changed) | a torn engine wedges GL apps | `put` (the shim is 135 KB, under the agent's 256 KiB request cap) |
| 9 | DeviceTools.swift:569 | same, for com.apple.SpringBoard.plist | CA_ENABLE_OGL / DYLD_INSERT_LIBRARIES for typein | B (when changed) | a bad plist means no SpringBoard | `put … 644` |
| 10 | DeviceTools.swift:575 (`usingAgent: false`, so SSH only) | `launchctl unload <it-pbd> …; rm -f <it-pbd>; launchctl unload <agent job> …; launchctl load <agent job>` | Retire it_pbd, (re)load the agent job | B (when the agent or job changed or the agent is absent) | the only step that must not run on the agent (a daemon cannot unload itself and reply); without sshd it cannot run | a new binary: `put` + `spawn [/bin/launchctl, stop, com.qemu.it-agent]` (KeepAlive relaunches it, proven §3c). Job/plist changes and it_pbd retirement: bake-time only (§6 P4) |
| 11 | DeviceTools.swift:595-601 | `set -e; sync; launchctl unload SB.plist; trap '…launchctl load SB.plist…' EXIT; …; cat > prefs.ltm-new; chown 501:501; chmod 600; mv -f …; sync` | Reload SpringBoard around a prefs rewrite (lock-button keys) | B (when changed) | the trap is the only thing that guarantees SpringBoard comes back | host-sequenced: `sync` → `spawn launchctl unload` → `put prefs 600 501:501` → `spawn launchctl load` (always, in a `defer`) → `sync` |
| 12 | DeviceTools.swift:656 | `cat > /tmp/ltm-itstatus.new && chmod 755 && mv … && /tmp/ltm-itstatus` | Foreground app name in the window title (first poll) | P | | agent `frontmost` (exists: bundle id + localized name, `Lock Screen` when locked). itstatus is redundant |
| 13 | DeviceTools.swift:658 | `/tmp/ltm-itstatus` | same, every 3 s (EmulatorController.swift:1282) | P (3 s) | the busiest shell user: one `/bin/sh` fork every 3 s | agent `frontmost` |
| 14 | DeviceTools.swift:684 | `cat > /tmp/ltm-itproxy.new && … && /tmp/ltm-itproxy on\|off` | Web proxy: write the Wi-Fi proxy settings (SystemConfiguration) | U (proxy revision) | | iPad design: PAC baked into the image's Wi-Fi service at build time (proxy, DIRECT fallback); on/off is host-side (itwebproxy mode). itproxy is dropped |
| 15 | DeviceTools.swift:723 | `cat > /tmp/ltm-ittrust.new && chmod 755 && mv …` | Stage the CA-trust helper | U | | see #16 |
| 16 | DeviceTools.swift:725 | `cat > /tmp/ltm-proxy-ca.der && /tmp/ltm-ittrust add\|remove /tmp/ltm-proxy-ca.der` | Trust the proxy CA | U | | stock `com.apple.mobile.MCInstall` (mc_mobile_tunnel is present on 7E18) via the iPad's `lockdown-mcinstall` child tool; the user taps Install once. Zero-touch fallback: `put` + `spawn` ittrust |
| 17 | DeviceTools.swift:730 | `sync` | Flush before restart (EmulatorController.swift:1191) and in the quit ladder (:1626) | U / F | memory powerdown-fixed: installs are lost without it on a hard stop | new agent op `sync` (prototyped, §3c). No lockdown service can flush the guest buffer cache |
| 18 | DeviceTools.swift:747 | `printf %s '<id>' > /tmp/sblaunch.id && /usr/local/bin/sblaunch` | Launch from the sidebar | U | a locked device is detected by a follow-up itstatus | agent `launch <id>` (exists: SBSLaunchApplicationWithIdentifier) + `lockstatus` (exists) |
| 19 | DeviceTools.swift:800 (`expecting:`, so SSH only) | `rm -f /tmp/ithalt && cat > /tmp/ithalt && chmod 755 /tmp/ithalt && exec /tmp/ithalt` | Quit: sync + unmount + halt | F (the agent's `halt` is tried first, EmulatorController.swift:1881) | SSH exits non-zero even on success, hence the marker | agent `halt` (exists: reboot2) → PMU confirmation. Else the stock power-off gesture (`qemu_ios_ui_powerdown`, fixed in feb556f471). ithalt is dropped |
| 20 | DeviceTools.swift:861-929 | the `guestRun` transport | all of the above | | the 30 s cap is shorter than the agent's 60 s tick budget | replace string commands with typed agent calls; delete the SSH branch |
| 21 | DeviceTools.swift:961 | `it-ssh-terminal.sh` → Terminal.app `ssh root@…` | Device ▸ Open Terminal | U | "no sshd on this image" | no stock equivalent. Hide it when the image has no shell (the iPad already does). Optional dev-only replacement: a syslog window (`syslog_relay`) |
| 22 | DeviceTools.swift:1010 (EmulatorController.swift:158) | agent `exec launchctl stop com.apple.mobile.lockdown` | Connection recovery | F | needs `/bin/sh` | `spawn [/bin/launchctl, stop, com.apple.mobile.lockdown]` |
| 23 | DeviceTools.swift:1016 | agent `halt` | Quit | U | none (no shell) | already clean |
| 24 | DeviceTools.swift:1022 (EmulatorController.swift:1017) | agent `orientation` | Rotation sync | P (250 ms) | 7E18-only SBGetUIOrientation ABI (ENOSYS elsewhere) | already clean on 3.1.3. On 4.2.1 use stock `springboardservices getInterfaceOrientation` (§2) |
| 25 | EmulatorController.swift:1052-1110 | iproxy + ssh `pkill -f /tmp/itorient; rm -f …; cat > /tmp/itorient && chmod 755 … && exec /tmp/itorient` | Orientation on images without the agent | F (long-lived session) | strands iproxy/ssh on quit (fixed by traps) | delete: every supported image has the agent |
| 26 | EmulatorController.swift:1298 | `qemu_ios_ui_paste` → machine `pasteboard` → QC_PB_* → it_agent | Paste into and copy from the guest | U | 40 s startup grace | stays (agent). pasteboardd is reachable only from inside the guest |
| 27 | USBMux.swift:218-240 | writes `session.env` for it-ssh-terminal.sh | Terminal | B | | goes away with #21 |

### 1b. Guest shell users outside the app process

| # | where | command | feature | replacement |
|---|---|---|---|---|
| 28 | imgtools/install-ipa.sh:141-176 `guest_open` | iproxy + ssh ControlMaster, `true` round trip, 90 s wait | tunnel for the fallback installer (non-baked images, via `qemu-ios-files/apps/install-app.sh`) | not needed: all shipping images are baked. Retire the script path for iPod images |
| 29 | install-ipa.sh:190 `install_shim` | `scp MBXGLEngine /tmp/…` + `cp -n $B/MBXGLEngine $B/MBXGLEngine.stock; cp … && chmod 755 …` | GL engine before a GL app | baked by the builder; upgraded through #8 |
| 30 | install-ipa.sh:226-255 placeholder add/remove | `test -x /tmp/sbdlicon \|\| scp …`, `/tmp/sbdlicon add\|cancel '<id>'` | placeholder | agent `dlicon` (#4) |
| 31 | LightTouchMac scripts/install-durability.py:102-166 | `sync; echo SYNCED`, `ls -d /var/mobile/Applications/*/*.app` | durability test | agent `sync`; installation_proxy `list` |
| 32 | LightTouchMac tests/check-media-components.py:52-68 | fake `ssh` shim | unit test of #6-#11 | follows the typed-call rewrite |
| 33 | qemu-ios tests/ipod/*.py (`guest_ssh`, 22 calls in regress.py; 7 files) | sblaunch `:lock-status`, respring, applaunch, crash-log pulls, test_agent_guest bootstrap | CI | agent `lockstatus`/`launch`/`spawn` for control, `crashreportcopymobile` (proven below) for logs |

Not shell users (for completeness): **time zone** is lockdown `SetValue TimeZone` through the
`lockdown-tz` child process (stock; lockdown-setvalue-trap). **Sound defaults** are
`set-sound-defaults.py`, a host-side plist edit at bake time. **Screenshots and recording** come from
the host framebuffer (QMP/dylib). **Icon order** uses stock `com.apple.springboardservices`
(SpringBoardIcons.swift). **List, install, uninstall and free space** use stock
installation_proxy/AFC/lockdown in-process (DeviceServices.swift). **Media staging** is stock AFC.
The **baked-image check** is an AFC stat of `.lt-guest-tools-v2`.

Counts: 27 app-side items (19 distinct `guestRun` command sites plus 2 transports, 1 Terminal, 1
session file, 1 orientation-over-SSH fallback, and 3 agent ops that are already shell-free), plus 6
outside the app. Of these, **20 app items and every item in §1b need `/bin/sh` today**, and 4 can only run over
SSH (#10, #19, #21, #25).

## 2. Replacement map

Preference order: stock lockdown service, then a hardware model, then an agent RPC (`spawn` with no
shell, or a direct API call inside the agent).

### Which stock services exist (evidence)

`/System/Library/Lockdown/Services.plist` checked against the binaries it names:

| service | 7E18 (3.1.3) | 8C148 (4.2.1) | answers on the emulated 3.1.3 (probe, §3a) |
|---|---|---|---|
| lockdown GetValue (+ `com.apple.disk_usage`) | yes | yes | **yes** (`3.1.3`, AmountDataAvailable …) |
| com.apple.afc (/var/mobile/Media) | afcd | afcd | **yes** |
| com.apple.mobile.installation_proxy | yes | yes | **yes** (`list --user`) |
| com.apple.mobile.house_arrest (VendContainer) | yes | yes | not exercised: the image has no user apps. The service is listed, InstanceLimit 5 |
| com.apple.syslog_relay | yes | yes | **yes** (160 lines in 8 s) |
| com.apple.crashreportmover / crashreportcopymobile | yes | yes | **yes** (`idevicecrashreport -k` copied a lockdownd report) |
| com.apple.mobile.notification_proxy | yes | yes | **yes** (post) |
| com.apple.springboardservices | getIconState, setIconState, getIconPNGData | + **getInterfaceOrientation**, getHomeScreenWallpaperPNGData | used by the app today |
| com.apple.mobile.MCInstall | mc_mobile_tunnel | yes | not probed (a profile needs a tap) |
| com.apple.mobile.mobile_image_mounter | yes | yes | **yes** (`list`: ImagePresent false). Mounting the 3.1.3 DDI: the AFC upload succeeds, then libimobiledevice 1.4.0 hangs up without sending MountImage ("Unknown error occurred, can't mount"). This is a client-side gap, so the guest mounter was not really tested |
| com.apple.mobile.diagnostics_relay | **listed, binary missing** (`/usr/libexec/mobile_diagnostics_relay` absent) | present: Restart, Shutdown, Sleep, … | **no**: "Invalid service" |
| com.apple.mobile.screenshotr | **not in the rootfs**. Only in the 3.1.3 DeveloperDiskImage (`/Developer/usr/bin/ScreenShotr` via ServiceAgents) | DDI only | **no**: "Invalid service" |
| com.apple.mobile.file_relay, mobilebackup, mobilesync, integrity_relay, misagent, debug_image_mount | yes | yes (+ mobilebackup2) | not probed |
| factory_proxy, software_update, system_profiler, purpletestr | listed, binaries missing | same (+ BTAutoPairing, PortableStorage, MDMService missing) | n/a |

### The map

| need | 3.1.3 | 4.2.1 | 2.x |
|---|---|---|---|
| list, install, uninstall, free space, staging | stock (in use) | stock | stock afc/instproxy (2.x lockdownd has no reboot/diagnostics, per ipod_touch_2g.c) |
| crash logs, syslog | stock crashreportcopymobile, syslog_relay | stock | stock |
| icon order | stock springboardservices | stock | check |
| orientation | agent `orientation` (7E18 ABI) | **stock springboardservices getInterfaceOrientation** | agent (new ABI) |
| time zone | stock lockdown SetValue (lockdown-tz) | stock | check |
| screenshots | hardware model (framebuffer) | same | same |
| halt / power off | agent `halt` → PMU confirm; hardware gesture `qemu_ios_ui_powerdown` as the fallback | **stock diagnostics_relay Shutdown**, then the gesture | the gesture (the 2.1.1 0x10 latch path) |
| restart | hardware reset after `sync` | stock diagnostics_relay Restart | reset after `sync` |
| sync | agent `sync` (new, direct `sync(2)`) | agent `sync` (or Shutdown/Restart, which sync) | agent |
| respring | agent `spawn /bin/launchctl stop com.apple.SpringBoard` | same | same |
| lockdown recovery | agent `spawn /bin/launchctl stop com.apple.mobile.lockdown` | same | same |
| launch app, lock state, foreground name | agent `launch` / `lockstatus` / `frontmost` (exist) | same, after re-verifying the SBS ABI | same |
| install placeholder | agent `dlicon` (new) | same | check the SBS symbol |
| component upgrade (GL shim, agent, typein, jobs) | bake-time; runtime via agent `get`/`put`/`chown`/`spawn launchctl` | same | same |
| music/video/photo commit | agent `put` + `spawn` itmedia/itphoto (stock frameworks, no shell) | same (re-test the MusicLibrary API) | same |
| proxy routing | PAC baked into the image at build time | same | same |
| proxy CA trust | stock MCInstall profile (one tap) | same | check MC on 2.x |
| clipboard | agent (QC_PB_*) | same | same |
| text input | §4 | §4 | §4 |
| Terminal | none (hidden) | none | none |

### Agent protocol additions (estimate)

| op | wire | status | size |
|---|---|---|---|
| `spawn` | body = argv as NUL-terminated strings, argv[0] absolute, stdin /dev/null, stdout+stderr captured, same 60 s/1 MiB limits as `exec` | **prototyped** (agent-ops.h, this branch) | ~40 lines C |
| `sync` | no args | **prototyped** | 2 lines |
| `chown` | `uid gid path`, lchown | new | ~10 |
| `unlink` | `path` | new | ~8 |
| `dlicon` | `add\|cancel <display-id>`, sbdlicon's SBS call on a worker thread | new | ~40 |
| retire `exec` and `kill` | `kill` shells out to `killall`; `exec` needs `/bin/sh` | delete | -40 |

That is about 100 lines of C plus host tests, and the agent stays about 70 KB. The Swift side
replaces string commands with typed calls, `GuestAgentTransport.perform(op:args:body:)` already
being the right shape: about 150 changed lines in DeviceTools.swift plus about 60 in
GuestFileSnapshot and its test.

## 3. Feasibility proofs (headless, overlay of nand-current.new, base never written)

Run: `timeout 570 python3 tests/ipod/probe_guest_services.py --keep` with the `ipad1` build of
qemu-system-arm, the build-native14 usbmuxd fork and Homebrew libimobiledevice 1.4.0. Wi-Fi was off
and there was no SSH anywhere. Boot to pairing took 38 s. Two runs gave the same results; the second added the load test. The
overlay is deleted on exit.

### 3a. Stock services on the emulated 3.1.3

See the "answers" column above. Seven services answer (lockdown, afc, installation_proxy,
syslog_relay, crashreport copy/mover, notification_proxy, mobile_image_mounter). `diagnostics_relay`
and `screenshotr` fail with "Invalid service", as the rootfs predicts.

### 3b. The "one lockdown session" limit does not hold on the current emulator

While `idevicesyslog` streamed:

- two rounds of 6 simultaneous sessions (2× ideviceinfo, 3× afcclient, 1× ideviceinstaller list)
  gave 12 of 12 OK;
- an install-shaped load of 3 parallel 8 MiB AFC uploads (2.8-3.7 MB/s each) plus 2× instproxy
  `list` and 2× lockdown queries gave 7 of 7 OK in 3.3 s wall time;
- lockdown still answered afterwards.

The historical "Could not start com.apple.afc: Invalid service" did not reproduce. The likely
explanation is the later OTG/usbmuxd fixes. Not tested under load: installd itself doing
`instproxy_install`, and right-after-uninstall. The app's global `DeviceGate` should stay until an
install-concurrency run passes. It is then a candidate for per-service locking.

### 3c. Prototype: shell-free agent RPC replacing `killall SpringBoard` and `sync`

`contrib/it-agent/agent-ops.h` in this branch: `agent_exec` is refactored into
`agent_start(argv, …)`, plus `spawn` (argv from the body, no shell) and `sync`.
`tests/ipod/test_agent_ops.py` covers them on the host under ASan/UBSan: argv passes through with
no shell interpretation (`$HOME;x` stays literal), relative and unterminated argv give EINVAL, a
missing binary gives ENOENT, and `sync` returns 0.

The guest run (same probe):

1. The baked agent answers `spawn` with -78 (ENOSYS): the old daemon, as expected.
2. The prototype is deployed with the agent's own `put /usr/local/bin/it_agent 755` and then
   `launchctl stop com.qemu.it-agent`. The request returns -54 (ECONNRESET, the daemon that
   answered is the one that stopped). KeepAlive relaunches the new binary, which claims the channel
   about 11 s later (the 10 s lease). This is also the SSH-free upgrade path for item #10.
3. `spawn [/bin/launchctl, list]` returns the job table (status 0). A bad subcommand returns status
   1 with stock launchctl's own stderr. `sync` returns 0.
4. **Respring:** `spawn [/bin/launchctl, stop, com.apple.SpringBoard]` returns 0. The launchctl
   SpringBoard row goes from PID 21 to PID 101. syslog_relay shows the new SpringBoard (`SpringBoard[89]`
   in the first run)
   starting (GLES shim, profile migration), the lock screen is drawn again
   (after-respring screenshot), and lockdown still answers. So `killall SpringBoard` (freeze
   `killall` + `/bin/sh`) is replaced by a stock binary, spawned directly.

## 4. Keyboards and text input

### 3.1.3 on iPod2,1: no stock end-to-end external-keyboard path

- **Kernel (7E18 kernelcache, prelink personalities):** a USB *host* HID stack is present:
  IOUSBFamily 3.5.0 (AppleUSBHub, IOUSBCompositeDriver), IOUSBHIDDriver "Generic Keyboard"
  (bInterfaceClass 3/1/1), IOHIDFamily 1.6.0 (IOHIDResource), and AppleSynopsysOTG2 with
  AppleSynopsysOTGHost strings. No AppleHIDKeyboard kext and no Bluetooth HID.
- **Bluetooth:** BTServer (MobileBluetooth-74.14) has A2DP, AVRCP and PAN, and **zero** "HID"
  strings.
- **Userland:** none of the hardware-keyboard API exists. `isInHardwareKeyboardMode`,
  `GSEventIsHardwareKeyboardAttached`, `GSEventSetHardwareKeyboardAttached` and
  `kGSEventHardwareKeyboardAvailabilityChangedNotification` all have 0 hits in the 3.1.3 shared
  cache, against 4/2/1/2 in iPad 3.2.2's. SpringBoard's only HID string is `PrintHIDEvents`. A USB
  keyboard could therefore enumerate in the kernel, if the single OTG port were switched to host
  mode (which ends usbmux), and its events would then be dropped.
- **What 3.1.3 does have:** GSEvent key events, the simulator's path: `_GSSendEvent`,
  `_GSEventGetKeyCode`, `_GSEventIsKeyRepeating`, plus UIKit `handleKeyEvent:`/`_handleKeyEvent:`.
  it_typein today uses UIKeyboardImpl in-process, and the machine can also tap the on-screen
  keyboard (`osk` property).

**Recommendation (3.1.3), after the P6 spike (below):** keep it_typein (bulk `insertText:`, one-key
`addInputString:`). It is invasive but proven. Out-of-process key events reach the app's keyboard
and are then dropped by the stock layout, so they cannot replace it. Where the agent is absent,
on-screen-keyboard taps stay the zero-guest-code fallback: faithful, but layout-bound and slow.

### P6 spike: out-of-process key events on 7E18 (result: does not work)

Prototype: agent op `keys [interval_ms]` in `contrib/it-agent/agent-keys.h` (body = UTF-8 text,
one event per UTF-16 unit, `\n` = Return, `\b` = Delete, at most 5 s of pacing per request, reply
= keys sent). Host check `tests/ipod/test_agent_keys.py`; guest probe `tests/ipod/probe_keys.py`
(overlay of `nand-current.new`, base untouched).

**The event path (7E18 shared cache; addresses are VAs in `dyld_shared_cache_armv6`):**

- GraphicsServices has no public key-event constructor on 3.1.3 (no `GSEventCreateKeyEvent`,
  `GSEventSendKeyEvent` or `GSKeyboard*`; iPad 3.2.2 has all of them plus
  `GSEventSetHardwareKeyboardAttached`). What exists is `_GSCreateSyntheticKeyEvent(unichar, up,
  repeat)` (0x3434f890) and `_GSPostSyntheticKeyEvent(CFString, up, repeat)` (0x34350278, which
  posts to the caller's *own* application port). They build a 0x3e-byte record: the 0x34-byte
  GSEventRecord header (type at 0, timestamp at 0x1c, infoSize = 10 at 0x30) plus key info: keyCode
  u16 at 0x34, charactersIgnoringModifiers u16 at 0x36, character u16 at 0x38, characterSet at
  0x3a, repeat u8 at 0x3c. Type is 10 (down) or 11 (up).
- `GSSendEvent(record, port)` (0x3434de44 → 0x3434dd18) sends a Mach message with id 0x7b,
  `COPY_SEND` to `port`, the record at +0x18, size aligned to 0x58, and stamps the timestamp. The
  receiver's `CreateWithMachMessage` (0x3434f2d4) takes any id other than 0x1f4 with size > 0x4b
  and overwrites senderPID from the audit trailer. Every app registers its purple port in bootstrap
  under its bundle identifier (`GSEventInitialize` → `GetIdentifierCString` →
  `GSRegisterPurpleNamedPort`), so a root daemon reaches it with `bootstrap_look_up`. SpringBoard
  answers as `com.apple.springboard`.
- UIKit: `_UIApplicationHandleEvent` → `-[UIApplication sendEvent:]` →
  `handleEvent:withNewEvent:` (0x3205480c). Its switch has **no case for 10/11** (types 10, 11
  and 12 fall through to `return 1`). The only key case is **13, kGSEventSimulatorKeyDown**
  (0x32056068) → `[[UIKeyboardImpl sharedInstance] handleHardwareKeyDownFromSimulator:event]`, which
  forwards to `m_layout` when a keyboard is up. Only `UIKeyboardLayoutRoman` implements it (0x32202cdc:
  0x7f → `deleteAction`, `\r` → Return, otherwise `sendStringAction:forKey:` with
  `GSEventCopyCharacters`, i.e. the character at 0x38). The base `UIKeyboardLayout` version is a bare
  `bx lr` (0x321efbf8).
- On 7E18 every stock keyboard is `UIKeyboardLayoutStar` (`UIKeyboardInputModeUsesKBStar` is a
  hard-coded input-mode table), which subclasses `UIKeyboardLayout`, not Roman. **So the last hop is
  a no-op.** iPad 3.2.2 instead has `-[UIApplication handleKeyEvent:]`, `-[UIKeyboardImpl
  handleKeyEvent:]`, hardware-keyboard mode and `GSKeyboardTranslateKey`: a real type-10/11 path
  that 3.1.3 lacks.

**Runtime evidence** (`probe_keys.py`, two runs, before and after merging `ipad1` f555749efe; same results). QEMU gdbstub
breakpoints (one extra diagnostic boot) show each `keys` event in Notes going
`PurpleEventCallback` (size 0x58) → `_UIApplicationHandleEvent` (type 13) → `sendEvent:` →
`handleEvent:withNewEvent:` (type 13) → case 13 → `-[UIKeyboardImpl handleHardwareKeyDownFromSimulator:]`
with `m_layout` = a `UIKeyboardLayoutStar` (superclass `UIKeyboardLayout`), and
`-[UIKeyboardLayoutRoman handleHardwareKeyDownFromSimulator:]` never hit.

| context | keys sent / queued | text that arrived |
|---|---|---|
| Notes, new note (keyboard up), 32 keys incl. é ✓ and 2 Deletes | 32/32 (1.3 s) | none (screenshot, uidump, notes.db) |
| Notes, 151 keys at 30 ms/key | 151/151 (5.3 s incl. RPC) | none |
| Notes, 150 keys unpaced | 150/150 (0.25 s, no send timeouts) | none |
| Safari address field (native UITextField, URL keyboard) | 29/29 | none, page never requested |
| Safari web text and password fields | not reached (the URL never loaded) | n/a: same case-13 → layout path |
| Spotlight (SpringBoard, `com.apple.springboard` port) | 5/5 | none |
| locked | -EACCES | n/a |

Transport, throughput and routing all work: no Mach send ever timed out (the port queue drains
faster than 600 keys/s), and the right app received every event. Nothing consumes them.

**Why not work around it:** the missing piece is one method on `UIKeyboardLayoutStar`. Adding it
(or any other consumer) needs code inside every UIKit app, which is exactly what it_typein already
is. There is no stock setting that selects the Roman layouts, and GSEvent types 10/11 have no
consumer. The only other text-bearing event, type 200 (`_processScriptEvent:`), loads a scripting
bundle that the stock rootfs does not have.

**Design, if it is revisited:** on 4.2.1, the IOHIDUserDevice keyboard below is the stock path. The
`keys` op's routing (frontmost display id → bootstrap purple port, bounded send, lock check) could
carry 4.x type-10/11 records to `-[UIApplication handleKeyEvent:]` if that path turns out to need no
hardware-keyboard attach. That is unverified, and it would still be a synthetic source where the HID
route is not.

### 4.2.1 on iPod2,1: a stock HID keyboard path exists, but no USB host

- **Kernel (8C148):** IOHIDFamily 1.6.2 (IOHIDResource → IOHIDResourceDeviceUserClient, i.e.
  IOHIDUserDevice), IOHIDEventDriver "HID Keyboard Driver", AppleHIDKeyboard (Apple Wired and
  Wireless Keyboard 2007-2009 maps) and AppleHIDKeyboardEmbedded (iAP B62 maps). **No IOUSBFamily**:
  only IOUSBDeviceFamily and AppleSynopsysOTGDevice. The iPad's CCK approach (host controller +
  usb-kbd) is therefore impossible on this kernel.
- **BTServer (MobileBluetooth-78.16):** `Server/hid/HIDProfile.cpp`, `HidService.cpp`,
  `Platforms/Purple/InputIOHIDClass.cpp`, "HID Host", "Received boot-mode keyboard report",
  `BT_DEVICE_TYPE_KEYBOARD`. No per-board exclusion strings were found. Runtime gating on N72 is
  unverified.
- **Userland:** SpringBoard has `isInHardwareKeyboardMode`, `handleSpecificHardwareKeyboard:`,
  `hardwareKeyboardAvailabilityChanged` and `_handleKeyEvent:`. GraphicsServices has
  `GSEventSetHardwareKeyboardAttached`. `/System/Library/KeyboardLayouts/USBKeyboardLayouts.bundle`
  is present.

**Recommendation (4.2.1):** the agent creates an **IOHIDUserDevice** (`IOHIDUserDeviceCreate`, stock
IOKit) whose descriptor and VendorID/ProductID match an Apple Wireless Keyboard (1452/569), and
forwards host reports with `hid <8-byte report>`. From there, kernel matching (AppleHIDKeyboard),
IOHIDEventSystem, SpringBoard's hardware-keyboard mode and UIKit are all stock. This is the same
entry point BTServer's InputIOHIDClass uses, so only the transport is synthetic. Estimate: 2-3 days
once 8C148 boots. Full-fidelity alternative: real BT HID in the HCI model (ACL, L2CAP, SDP, HIDP,
pairing; hw/arm/ipod_touch_bt.c is command-complete only today). That is 2-4 weeks and not
justified by any visible difference.

## 5. Networking

| | 7E18 | 8C148 |
|---|---|---|
| USB Ethernet *host* (adapter) drivers: AppleUSBEthernet*, CDC ECM/NCM, ASIX, RTL | **none** (0 string hits; the 6 "NCM" hits are PPPIOC*ASYNCMAP) | **none** |
| USB host stack | IOUSBFamily present, but no class driver for network | absent |
| Device-side "AppleUSBEthernet" function | defined in USBDeviceConfiguration.plist (`standardMuxPTPEthernet`, iPhones' tethering, served from userland through IOUserEthernet/misd, with no kext personality matching it), but **iPod2,1 → `standardMuxPTP`** | same mapping |
| Wi-Fi | AppleBCMWLAN + AppleS5L8900XSDIO + IO80211Family | same |

Wi-Fi through the emulated BCM4325 SDIO chip (hw/arm/ipod_touch_sdio.c, slirp `wifi0`) stays the
only stock network path. It is proven on 3.1.3 (regress `wifi`, net-proof-safari). It is expected
to hold on 4.2.1 because the driver set is the same, but that is unverified until 8C148 boots.
Enabling USB Ethernet would mean editing USBDeviceConfiguration.plist and relying on misd's
tethering on a device Apple never shipped it for. Not recommended.

## 6. Phased plan

| phase | work | effort | exit check |
|---|---|---|---|
| P0 (this branch) | inventory, service probe, `spawn`/`sync` prototype, host tests | done | §3 |
| P1 agent protocol | land `spawn`/`sync`; add `chown`, `unlink`, `dlicon`; delete `kill`; keep `exec` only until P4 | 1.5-2 d | test_agent_ops.py, test_agent_guest.py with no SSH bootstrap |
| P2 app, typed calls | replace every row of §1a with the map: GuestFileSnapshot → `get`; upgrades → `put`/`chown` + `spawn launchctl`; frontmost/launch/lockstatus; dlicon; media commit via `spawn`; `sync`; respring; recovery. Delete the SSH branch of `guestRun`, runOrientationWatch and the itstatus/sblaunch/ithalt/itorient staging. Hide Terminal when the image has no shell | 2-3 d | check-media-components.py, check-app-launch.py, a manual pass by Sam |
| P3 proxy and trust as on the iPad | PAC in the image's Wi-Fi service at build time; MCInstall profile through `lockdown-mcinstall`. Verify ManagedConfiguration-313.17 accepts it | 1 d | regress `webproxy` |
| P4 image without a shell | IPSW builder stops consuming ipod2g-shell.txt (freeze, OpenSSH, OpenSSL) and the sshd job; bake it_agent, it_typein, the GL shim, markers, sound defaults; marker v3 = "agent, no shell"; split `guestShell` into hasAgent/hasShell in the app | 0.5-1 d | nand_manifest.py diff (only the shell package gone) |
| P5 CI without SSH | regress.py and friends: lockstatus/launch/respring via the agent, crash logs via crashreportcopymobile, unlock via `lockstatus` + the swipe | 1-2 d | full `regress.py`, all checks green on a no-shell image |
| P6 3.1.3 text input | GSEvent `keys` spike (§4): **done, does not work** (events reach UIKeyboardImpl, the stock Star layout drops them). Keep it_typein | done | `probe_keys.py`, §4 |
| P7 4.2.1 | after 8C148 boots: re-verify each SBS ABI (orientation moves to stock springboardservices, halt/restart to diagnostics_relay); IOHIDUserDevice keyboard; Wi-Fi check | 3-5 d | same probe on 8C148 |
| P8 2.x (5F138) | after it boots: agent SBS ABIs; gesture power-off; the service set is smaller (no diagnostics) | 2-3 d | same probe |

Total for 3.1.3 feature parity without a shell: **about 6-9 working days (P1-P5)**, P6 is done (negative).

### What stays guest-side, and why

- **it_agent** (one root daemon, cp15 channel, no network listener): clipboard (pasteboardd is only
  reachable from inside), `sync` (the guest buffer cache), SpringBoardServices calls (launch,
  frontmost, lock state, 7E18 orientation, the download placeholder; 3.1.3's springboardservices
  relay exposes only icon state), launchd control (stock `/bin/launchctl`, spawned), and the media
  database commits.
- **itmedia/itphoto**: one-shot helpers spawned by the agent, not a shell. They use MusicLibrary
  and PhotoLibrary, for which no stock host-side service exists on 3.x (iTunes wrote a hashed
  iTunesDB). They stay separate processes so a framework crash cannot take the agent down.
- **The MBXGLEngine shim**: the MBX GPU is not modelled.
- **it_typein on 3.1.3**: there is no stock keyboard path, and P6 showed that out-of-process key
  events are dropped by the stock layout.
- Everything else (installs, logs, crash reports, icons, time zone, screenshots, power, networking)
  is stock services or hardware models.

## Reproducing the evidence

- Services: `plutil -p /System/Library/Lockdown/Services.plist` on the mounted rootfs, then
  `test -e` each ProgramArguments[0].
- Kernel personalities: parse the prelink XML (7E18: the `<array><dict>…` run in `__PRELINK_INFO`;
  8C148: `_PrelinkInfoDictionary`) and filter on `hid|usb|otg|ether|keyboard|bluetooth`.
- Userland: `LC_ALL=C grep -c -a -F <symbol> dyld_shared_cache_armv6`, and `strings` on SpringBoard
  and BTServer.
- Runtime: `tests/ipod/probe_guest_services.py`.
- P6: `timeout 570 python3 tests/ipod/probe_keys.py --out DIR` (screenshots, uidump, notes.db).
  The breakpoint trace used QEMU's `-gdb` stub with Z0 breakpoints at the addresses in §4 (the
  shared cache is mapped at the same VA in every process); step over each hit before continuing.
