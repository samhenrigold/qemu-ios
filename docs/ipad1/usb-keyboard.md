# iPad 1 (7B500): USB keyboard on the host controller, next to usbmux

**Answer: yes, both at once, using stock kernel code.** When `usb-complex` has the DT property
`hsic-enabled`, `AppleS5L8930XUSBArbitrator` publishes the host controllers (EHCI + OHCI0) when it
starts and never tears them down on a cable change. The OTG device side (usbmux) keeps following
the cable as it does today. On a real board, `hsic-enabled` is how the baseband's HSIC link stays
up on EHCI while the dock port runs in device mode, so the kernel is built for exactly this
coexistence. The emulator only has to present the controllers and add that one property.
The emulator changes that follow from this are listed under "Implemented" below.

The traces are from `7B500/dec/kernelcache.mach`, disassembled with `work/kc.py`. Kext bases
come from `work/kexts.txt`.

## How the arbitrator decides

Personalities: `AppleS5L8930XUSBArbitrator` (com.apple.driver.AppleS5L8930XUSB, c0481000) matches
`usb-complex,s5l8930x` and subclasses `AppleEmbeddedUSBArbitrator` (com.apple.driver.AppleEmbeddedUSB,
c0352000). It publishes an `AppleEmbeddedUSBNub` for each child whose **`cable-type`** equals the
type being published (`_publishNubs` c0353a8c):

| child | cable-type | driver that matches the nub |
|---|---|---|
| `usb-device` (OTG, 0x86100000, IRQ 0x0d) | 1 | `AppleSynopsysOTGDevice` (usb-device,s5l8900x) |
| `usb-ehci` (0x86400000, IRQ 0x0e) | 2 | `AppleUSBEHCIARM` (usb-ehci,s5l8930x), generic EHCI. `AppleSynopsysUSBEHCI` only matches `usb-synopsys_ehci` |
| `usb-ohci0` (0x86500000, IRQ 0x0f, rh-ports 1) | 2 | `AppleUSBOHCIARM` (usb-ohci,s5l8930x) |
| `usb-ohci1` (0x86600000, IRQ 0x10) | 3 | only when `start-ohci1` is set; not published |

Cable types are OSSymbols set up by AppleARMPlatform (c02d3378): `Detached`, `USBHost`,
`USBHostAltConfig` (we are the device, as with a Mac), `USBDevice`, and `USBDeviceNeedsAuthentication`
(we are the host, as with a CCK). They reach the arbitrator as the `AppleUSBCableType` property.

**Base class `_handleUSBCableTypeChange` (c0353b88), no HSIC:**
- `USBHost*`: publish the device nubs (field +0x70) on first use; after that, message them
  "attach" (0xe3ff8100, arg 1).
- `USBDevice*`: publish the host nubs (+0x74).
- Going from one attached type to another without passing through `Detached`: terminate the host
  nubs and message the device nubs "detach" (arg 0).
- `Detached`: terminate the host nubs and message the device nubs "detach".

So without HSIC the two sides alternate. The OTG nub is never destroyed, but it is told to detach
whenever host mode comes up.

**`start` (c0353cdc):**
- The boot-arg **`force-usb-host`** (read through `PE_parse_boot_argn`, not the DT) forces
  `USBDevice` (host mode).
- Otherwise a `no-pmu` property on the node forces `USBHost` (device mode).
- Otherwise the arbitrator waits for the power source's `AppleUSBCableType`.

**Subclass (AppleS5L8930XUSBArbitrator):**
- **`handleStart` (c0482650):** maps `USB_CTL` (the `usb-complex` reg, 0xbf108000). If
  **`hsic-enabled`** is present (only its presence is checked), it sets the flag +0x80, sets
  `USB_CTL |= 4` and calls `AppleUSBPhy->enable(1)` (c04822f0), then **publishes the host nubs
  (`_publishNubs(2)`) immediately**.
- **`_handleUSBCableTypeChange` (c0482358):**
  - `USB_CTL &= ~1` on every change, then `|= 1` for the `USBDevice*` types, so bit 0 selects
    the host PHY mux.
  - With the HSIC flag set, **the host nubs are never terminated**.
  - `USBDevice*` changes only update the recorded state.
  - `Detached` messages the device nubs "detach".
  - `USBHost*` falls through to the base class, which publishes or attaches the device nubs.
- **Power-state overrides (c04822a8/c04822c0):** skipped when the flag is set.

Does EHCI need the OTG core in host mode? No. EHCI and OTG are separate register blocks. The only
coupling is `USB_CTL` bit 0 (the PHY mux on real silicon), and the emulator has no shared PHY to
mux.

## What a Camera Connection Kit is to the iPad

The CCK is a 30-pin accessory. The dock's accessory detection tells the power source it has a
host-side cable, which it reports as `USBDevice` or `USBDeviceNeedsAuthentication`. The
`NeedsAuthentication` variant is the iAP-authenticated case; iapd's authentication lives in user
space (keyboard-and-network.md §1). The arbitrator then brings up EHCI/OHCI, and a keyboard
enumerates through the stock stack: `IOUSBHIDDriver` "Generic Keyboard" (class 3/1/1), then
IOHIDFamily's `AppleHIDKeyboardEventDriver`, then UIKit.

Which kext publishes `AppleUSBCableType` on K48 (the `AppleD1815PMUPowerSource` string is the iPhone
PMU) was not traced. With `hsic-enabled` it doesn't matter for the host side.
`AppleEmbeddedUSBDevice`'s `Authenticated` gate (c03540e8) only applies to the Apple iPod-accessory
USB device (`AppleEmbeddedUSBiPod`, 05ac:12xx), not to HID.

## Designs

**a. Both at once (implemented).** Add `hsic-enabled` to `usb-complex` and model EHCI + OHCI0. The
host side is up from arbitrator start, and usbmux is unaffected. "Plugging" the keyboard means
attaching a QEMU `usb-kbd` to the EHCI bus, at startup or hot-plugged with `device_add`, and
unplugging means `device_del`. The guest sees a normal USB connect and disconnect on the root hub,
which is exactly what a CCK plus keyboard looks like after the controller is up. The only thing
that isn't stock is the one DT property, which a 3G board's DT would carry anyway.

**b. Mode switch (not built).** Without `hsic-enabled`, the app changes `AppleUSBCableType` between
`USBHost` and `USBDevice`, like a CCK plug and unplug. That means modelling whatever K48 power-source
path publishes it (not traced). Every switch into host mode tells the OTG side to detach, so usbmux
drops (the app loses lockdown/AFC) until the CCK is "unplugged". Worse in every way than (a), so
it's kept only as the answer to "can it be done like hardware".

## Implemented (branch ipad1-kbd)

- `imgtools/ipad1_kboot.py`: `DeviceTree.add()` appends a property; the blob grows, so the memory
  layout now comes after the DT edits. `build()` adds `arm-io/usb-complex/hsic-enabled` (empty).
  The self-check covers it.
- `hw/arm/ipad1.c`:
  - `exynos4210-ehci-usb` (EHCI caps at +0, op regs at +0x10) at 0x86400000, IRQ 0x0e.
  - `sysbus-ohci` (1 port) at 0x86500000, IRQ 0x0f.
  - `USB_CTL` stays in the unimp window; the kernel only read-modify-writes it.
- Attach a keyboard with `-device usb-kbd,bus=<ehci bus>` (see the boot test for the bus name).
  QEMU's `usb-kbd` exposes a high-speed descriptor set, so it attaches to the EHCI root port
  directly without an OHCI companion.

## Boot test (2026-09-27, merged ipad1 ba6cd0b812, golden-pristine clone)

`-machine ipad1,kboot=<rebuilt> -device usb-kbd,bus=usb-bus.0`. `usb-bus.0` is the EHCI bus, and
`info usb` shows the keyboard at 480 Mb/s. Serial shows:

- `AppleS5L8930XUSBArbitrator::handleStart : hsic-enabled`, then two `_publishNubs : nub published`.
- The EHCI and OHCI root hubs start (`AppleUSBHub ... USB Generic Hub @ 1 (0x1000000)` and `(0x2000000)`).
- The keyboard enumerates: `USB HID Interface #0 of device QEMU USB Keyboard @ 2`.
- The device side still comes up after that: `cableType: USBHost`, then `Connected to a USB Host`,
  then `AppleSynopsysOTGDevice::start`. All four configurations register, including `AppleUSBMux`,
  and the built-in host configures configuration 3 ("PTP + Apple Mobile Device"). This run did not
  include a usbmuxd bridge, so the lockdown round trip over the bridge was not exercised.

SpringBoard reaches the home screen with the stock alert "The attached USB device is not
supported." Dismiss it, open Notes, tap the note body, and type through QEMU. The text lands in the
note, and no on-screen keyboard appears: "Hello from a USB keyboard!" is visible in the note in a
screendump.

**The alert comes from MobileStorageMounter, not the kernel.** It is `UNSUPPORTED_FAILURE` in
`/System/Library/CoreServices/MobileStorageMounter.app` (launchd job
`com.apple.mobile.storage_mounter`).
- The mounter registers `IOServiceAddMatchingNotification(IOServiceFirstMatch,
  IOServiceMatching("IOUSBInterface"))` with no further filter (0x3358).
- For each batch of new interfaces, the callback (0x2e0c) reads `bInterfaceClass`, `bInterfaceSubClass`
  and `bInterfaceProtocol`. It treats class 1 (audio), class 8 (mass storage) and 6/1/1 (PTP camera)
  as supported. If the batch held at least one interface and none of them was supported, it raises
  the alert (0x1828).
- A HID keyboard (3/1/1) always trips it. That matches what CCK keyboard users reported on a real
  3.2 iPad: dismiss the alert and the keyboard works. It isn't caused by the root hubs, the port, or
  anything else in our model.

Every way to suppress it is non-stock:
- Put a fake audio, storage or PTP interface in the same notification batch as the keyboard.
- Patch or disable the mounter job. Disabling it also loses CCK photo import.

So a stock boot with a USB keyboard shows this alert once per attach, as a real iPad does, and it is
left as is. If the app wants it gone, it can auto-tap Dismiss after attaching the keyboard.

Harness notes:
- `usb-kbd` activates itself as the head keyboard handler, so while it is attached, host keys go
  to it and not to the machine's button handler (L / Shift+H / - / =). Drive the buttons some other
  way, such as the GPIO/QMP path, when a keyboard is attached.
- Touch is an absolute handler. HMP `mouse_move` is relative and never reaches it, so use QMP
  `input-send-event` with `abs` x/y (0..32767 over the 1024x768 scanout) and a `btn` event.
- QMP `input-send-event` with a `device` argument aborts QEMU under `-display none`
  (`qemu-fixed-text-console.device` not found). Leave `device` out.

Regression on this branch: `tests/ipod/regress.py` default tier: boot, fsck, persist and agent
PASS; appinstall, applaunch, gles and audio SKIP because Harness.ipa is not built in this worktree.

Remaining notes:
- The fallback remains `contrib/ipad1-hidbridge` (keyboard-and-network.md §1).
