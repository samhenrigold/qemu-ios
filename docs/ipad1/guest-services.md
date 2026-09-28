# Guest services on the iPad 1 (iOS 3.2.2)

Policy: keep the guest as close to stock as possible. The iPod needed a guest
agent because its USB was limited. The iPad on 3.2.2 has USB Ethernet and the
full set of lockdown services, and the emulator models the touch screen,
accelerometer, PMU shutdown and (planned) a Bluetooth keyboard. Every iPod
guest tool was put into one of three buckets:

- **(a)** replaced by a stock iOS service over USB
- **(b)** replaced by a hardware model
- **(c)** stays in the guest

Only the (c) items are built.

Service availability was checked against the 7B500 system volume
(`/System/Library/Lockdown/Services.plist`) and the 3.2.2 DeveloperDiskImage
(`/Library/Lockdown/ServiceAgents`). The host reaches all of them through usbmuxd
and lockdownd (libimobiledevice, as LightTouchMac already does for the iPod).

## The mapping

| iPod tool / agent op | Bucket | iPad replacement | How LightTouchMac reaches it |
|---|---|---|---|
| `it_agent put`/`get`/`getrange` | a | `com.apple.afc` (media jail); `com.apple.mobile.house_arrest` (app containers) | `afc_*`, `house_arrest_*` |
| `it_agent exec` | dropped | nothing stock; the only real users were install and debugging, both covered below | — (ssh on the jailbroken base for development only) |
| app install, `sbdlicon` placeholder, `isprogress` | a | `com.apple.mobile.installation_proxy` (its status callbacks carry `PercentComplete`) | `instproxy_install` with a status callback; the progress bar comes from the host |
| `sblaunch` / `it_agent launch`, `kill` | a | DDI `com.apple.debugserver` (+ `.applist`), after `com.apple.mobile.mobile_image_mounter` mounts the 3.2.2 DDI | the Xcode path: mount the DDI, then gdb-remote `A`/`k` (idevicedebug) |
| `itstatus` / `frontmost`, `lockstatus` | **c** (2026-09-28) | no stock query answers the foreground app, and the window title needs it (the user taps icons; the host does not know what runs) | `it_agent` `frontmost`/`lockstatus` over the cp15 `QC_AG_*` channel, as on the iPod |
| `itorient` / `orientation` | a | `com.apple.springboardservices` `getInterfaceOrientation` (in 3.2.2's springboardservicesrelay, absent on the iPod's 3.1.3) | `sbservices_get_interface_orientation` |
| screenshots (iPod read the framebuffer) | a (or host framebuffer) | DDI `com.apple.mobile.screenshotr` | `screenshotr_take_screenshot`; the display model's framebuffer is cheaper and needs no DDI |
| logs | a | `com.apple.syslog_relay`, `com.apple.crashreportcopymobile` | `syslog_relay_*`, `afc` on the crash-report service |
| `settime` | b | PMU RTC model, seeded from host time | machine property / RTC model |
| `ithalt` / `halt` | b | PMU shutdown model; the guest powers off through the stock power-button hold and slide (touch model). `mobile_diagnostics_relay` on 3.2.2 only answers `GasGauge`, with no Shutdown/Restart | GPIO button + touch injection; wait for the PMU power-off |
| `itbattery` | a/b | gas-gauge/PMU model; readable back through lockdown's battery domain and `diagnostics_relay` `GasGauge` | `lockdownd_get_value(domain "com.apple.mobile.battery")` |
| `sbunlock` | b | touch model: the host performs the slide-to-unlock gesture | touch injection |
| `it_typein.dylib`, `it_kbd_agent` (text input) | b | Bluetooth keyboard model (planned). The on-screen keyboard through touch works now | HID events |
| accelerometer / shake | b | accelerometer model | machine properties |
| **pasteboard** (`it_pbd`, `it_agent` clipboard) | **c** | see below | cp15 channel `QC_PB_*` |
| GLES engine shim | c | built separately (userland-gl-gaps) | — |
| `MBXGLEngine` | iPod-only | PowerVR MBX; the iPad is SGX | — |

## Why the pasteboard stays in the guest

pasteboardd holds the live pasteboard and UIPasteboard is its only client, so
something in the guest has to call UIPasteboard. I checked every way a stock
service might reach it:

- **Lockdown services:** no binary behind a `Services.plist` entry references
  the pasteboard (strings search over each ProgramArguments[0]).
- **MobileSync / SyncAgent:** syncs Calendars, Contacts, Bookmarks and Notes
  only. There is no clipboard data class.
- **DDI 3.2.2:** the only pasteboard strings are in Shark's Mac-side nibs.
  debugserver could in principle force a UIPasteboard call inside an attached
  app, but that means stopping a foreground app for every clipboard sync.

The guest side is therefore `it_pbd`, the older clipboard-only daemon, not
`it_agent`. It has no exec, file or UI routes: it only polls the host
clipboard and publishes guest pasteboard changes, as `public.utf8-plain-text`.
It is installed as a root launchd job and leaves SpringBoard untouched: no
`DYLD_INSERT_LIBRARIES`, no environment changes.

**Since guest package serial 2 (2026-09-28)** the iPad runs `it_agent` instead: the window title needs
the foreground app, which no stock service reports (the table above). The agent carries the same
pasteboard bridge (`QC_PB_*`), so `it_pbd` is dropped from the k48 packages; two pasteboard daemons would
race. The ipad1 machine routes `QC_AG_*`/`QC_UI_*` to `hw/arm/ipod-agent.c` like the iPod machine. The
agent is built for armv7 by `contrib/ipad1-guest/build.sh`. Checked on 7B500 and 8C148: `ping`,
`frontmost` (Home Screen, then Safari), `lockstatus`, the pasteboard handoff (`pasteboard-status`
"delivered ... (agent: alive)"); `dlicon` answers 0 but SpringBoard shows no placeholder on 3.2/4.2.

## it_ethlink: the USB Ethernet link (2026-09-27)

`AppleUSBEthernetDevice` starts its output queue and arms its first bulk read only in
`setProperties({"LinkStatus": 1})`. On a tethering iPhone, configd's USBEthernetSharing makes that
call. A Wi-Fi iPad has no tethering, so en1 stays down even when usbmuxd bridges it.
`contrib/it-ethlink/it_ethlink.c` is a root launchd daemon that does what USBEthernetSharing would, with
stable IOKit API only:

1. It matches the `AppleUSBEthernetDevice` service.
2. It registers for its `IOGeneralInterest` messages.
3. On every message it calls `IORegistryEntrySetCFProperty(svc, "LinkStatus", 0)`, then the same with
   `1`. The host's switch to alt setting 1 arrives as one of these messages (0xe3ff8201 on 7B500).
   With alt 0 the kext only records the value, so the kick after alt 1 is the one that starts the
   queue.

It needs no entitlement or privilege beyond running as root: both calls return 0, and the kext logs
`AppleUSBEthernetDevice::setProperties: Link Status - 1`. It writes its own log to `/dev/console`
(serial). A property-driven variant, which waits for `HostAttached` to become true, does not work:
the property appears only after alt 1, and it reads false.

This replaces the 7B500-only kernel patch, which is gone from `ipad1_kboot.py` (2026-09-27); the
bundle is a stock kernel. Proof, with the regenerated stock
`7B500/k48-kboot.bin` and the re-baked golden-pristine, using a4-net's
`tests/ipad1/regress.py --checks boot,net`: both PASS ("guest fetched /regress-….html over en1
(usbmuxd slirp)"), and Safari renders the page (screens/ethlink-safari.png). The patched bundle is kept
as `7B500/k48-kboot-ethpatch.bin`, and the previous golden as `userland/golden-pristine.old-ethpatch`.

## Build and bake

```
contrib/ipad1-guest/build.sh                  # build/ipad1-guest/{it_pbd,it_ethlink}, armv7, ldid -S
imgtools/ipad1_rootfs.py bake OUT/pristine    # both into /usr/local/bin + their com.qemu.* jobs, root:wheel via the catalog
imgtools/ipad1_nand.py build ...              # rebuild the store
```

- `contrib/armv6-toolchain/armv6.sh` takes `GUEST_ARCH=armv7` (the 3.2 SDK,
  cpusubtype 9, `-marm`). `mkold.py` turns `LC_MAIN` into `LC_UNIXTHREAD`, so
  this is a plain executable, not the dylib-in-`sleep` workaround from
  hw2-regs/README-native-code-on-3.2.2.md. That has not yet been proven on 3.2.2
  hardware.
- The ad-hoc signature needs `amfi_allow_any_signature=1`.
- `build_nand.set_owner` now handles the 7B500 system volume's 8 KiB
  allocation blocks.

## Host side (done)

- `hw/arm/guest-pasteboard.c` / `include/hw/arm/guest-pasteboard.h` hold what
  used to be the iPod machine's `pb_*` state and code: the `GuestPasteboard`
  struct, the `QC_PB_*` handler (`guest_pb_call`), the host clipboard peer,
  the no-agent warning timer and the four properties (`pasteboard`,
  `guest-pasteboard`, `pasteboard-agent`, `pasteboard-status`). Both machines
  embed one and call `guest_pb_init` from instance_init. The iPod's
  `guest-services.c` routes `QC_PB_*` there; behaviour and property text are
  unchanged.
- ipad1 already registered `QEMU_CALL` for the GLES shim (`ipad1_qemu_call`);
  its dispatch now also answers `QC_PB_*`. Everything else stays unanswered.
- LightTouchMac needs no change: its Paste Text to Guest command sets the
  machine's `pasteboard` property (`qemu_ios_ui_paste`), and guest copies reach
  the Mac clipboard through the QEMU clipboard peer, as on the iPod.

### Round trip (2026-09-27)

A store built from `userland/pristine` (the golden-pristine inputs) plus
`bake`, booted with `amfi_allow_any_signature=1 cs_enforcement_disable=1`.
`AMFI: Invalid signature but permitting execution` is it_pbd starting.

1. `pasteboard-agent` → `alive: 1267 polls`.
2. `qom-set pasteboard "Hello iPad, from the host! #1 (a-b) $5"`, then
   `pasteboard-status` → `delivered: 38 bytes`.
3. In Notes, the Paste callout appeared and pasting inserted the exact text,
   punctuation included.
4. Typing `Qwer` on the on-screen keyboard, then double-tap and Copy, gave
   `guest-pasteboard` → `Qwer`.

Everything else in the table is USB (usbmuxd → lockdownd) or a hardware model,
and needs no channel.
