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
| `itstatus` / `frontmost`, `lockstatus` | dropped | no stock query. The host already knows what it launched, and the lock state follows from the screen and the modelled lock button | — |
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

## Build and bake

```
contrib/ipad1-guest/build.sh                  # build/ipad1-guest/it_pbd, armv7, ldid -S
imgtools/ipad1_rootfs.py bake OUT/pristine    # /usr/local/bin/it_pbd + com.qemu.it-pbd job, root:wheel via the catalog
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

## Host side for the ipad1 machine (pasteboard only)

**Do not boot a baked image until items 1 to 3 below exist.** Unregistered, the
`mcr p15, 3, rX, c15, c15, 0` UNDEFs, and `it_pbd` crash-loops under
KeepAlive. That is harmless to the rest of the system, but it fills the log.

1. **Register the channel.** In `hw/arm/ipad1.c`, register the `QEMU_CALL`
   cp15 register on the Cortex-A8, with the same ARMCPRegInfo as
   `ipod_touch_2g.c`: opc1 3, crn 15, crm 15, opc2 0, `PL0_RW`, `ARM_CP_IO`,
   AA32, `qemu_call`.
2. **Move the pasteboard state out of the iPod machine.** In
   `guest-services.c`, move the `pb_*` fields out of `IPodTouchMachineState`
   into a small struct both machines embed. Pass it through `ri->opaque`.
3. **Trim the ipad1 dispatch to the pasteboard.** On ipad1 the dispatch should
   answer only `QC_PB_*` (the agent, keyboard and GLES cases return
   `QC_ERR_ENOSYS`). Add `guest-services.c` to `CONFIG_IPAD1`.
4. **Add the machine properties.** Give the ipad1 machine the `pasteboard` and
   `pasteboard-status` properties the frontend already uses.

Everything else in the table is USB (usbmuxd → lockdownd) or a hardware model,
and needs no channel.
