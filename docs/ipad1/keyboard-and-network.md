# iPad 1 (7B500): hardware keyboard, USB Ethernet, Wi-Fi

This is read-only research. Nothing here has run in QEMU yet. Sources:
`7B500/dec/kernelcache.mach` (strings and the prelinked kext plists),
`7B500/dec/DeviceTree.txt`, `mnt-rootfs`, the real-device log `hw2/dmesg.txt`,
`hw/arm/ipad1.c`, `hw/arm/ipod_touch_bt.c`, `hw/arm/s5l8930_iop.c`, and
`usbmuxd-qemu/usbmuxd/src/usb-qemu.c`.

## Recommendations

| Feature | Recommendation | Cost | Keeps usbmux? |
|---|---|---|---|
| Keyboard | A guest helper that creates an `IOHIDUserDevice` (the same API that BTServer and iapd use) and gets HID reports from the host over usbmux | 1-2 days | yes |
| Keyboard, fallback | QEMU `ehci-sysbus` + `usb-kbd` at the A4 host complex, with the arbitrator forced into host mode | 2-4 days | **no**, the PHY is shared |
| Keyboard, not recommended | iAP keyboard dock (needs Apple accessory auth) or a BT HID keyboard (needs an ACL/L2CAP/HID stack in `ipod_touch_bt.c`) | 1-2+ weeks | yes |
| Network | USB Ethernet: pick configuration 4 in `usb-qemu.c`, pump the Ethernet bulk pipes into libslirp, and add a DHCP service for the interface to the NAND image | 3-5 days | yes |
| Wi-Fi | Don't build it. It means a fake BCM4329 SDIO dongle speaking the dhd/CDC ioctl protocol behind the IOP SDIO ring | 3-6 weeks | n/a |

## 1. Hardware keyboard

### How a keyboard reaches the OS on 3.2

The OS has three entry points, and all three end at an IOHIDFamily `IOHIDDevice`:

1. **iPad Keyboard Dock (30-pin, iAP).** `IOAccessoryManager.kext` has these
   personalities: `Dock` (`IONameMatch dock,30pin`), `IAP Serial Port`
   (`IOAccessoryPortSerial` on `AppleOnboardSerialSync`, the DT node `iap` under
   `uart1`), and `IAP USB Port` (`IOAccessoryPortUSB` on the
   `USBDeviceFunction=IapOverUsbHid` interface, which is the `IOAccessoryPortUSB::start`
   line in dmesg). The kext only carries bytes. The iAP protocol lives in
   `/System/Library/PrivateFrameworks/IAP.framework/Support/iapd`.
   `-[IapHIDDescriptor initWithReportIndex:andVID:andPID:andCountryCode:andTransport:andHIDDescriptor:]`
   takes the HID descriptor the accessory sends and calls
   `IOHIDUserDeviceCreate`/`IOHIDUserDeviceHandleReport`. In the kernel,
   `AppleHIDKeyboardEmbedded` has the personality "iAP B62 ISO Map" (VID 0x5ac,
   PID 0x23d, `FnFunctionUsageMap`) that matches the dock keyboard's
   `IOHIDInterface`. **The blocker:** iapd authenticates the accessory. The
   strings include the Apple iPod Accessories CA certificates,
   `SecTrustCreateWithCertificates`, and FEE signature code under
   `IapAuthentic/`. We can't produce a valid accessory certificate or signature.
   `_IapAllowFakeAuthV1ForVPort` exists, but it looks like it only applies to
   virtual ports. Getting past auth would mean patching iapd.
2. **Bluetooth HID.** BT is on UART3 (0x82800000, `bluetooth,n88`, H4 at 3 Mbaud,
   bt_reset on TCA6408 pin 0; see ref-a4-board.md §8). `BTServer`
   (MobileBluetooth-75.26, `Server/hid/HIDProfile.cpp`, `InputIOHIDClass.cpp`)
   also ends in `IOHIDUserDeviceCreate`. Our `ipod_touch_bt.c` only answers
   command-complete: no ACL, no L2CAP, no link control. `ipad1.c` wires up only
   UART0 today. For a keyboard we would need a fake remote device: a
   pre-seeded link key and HID info in the MobileBluetooth device plist, a
   Connection Request, a Link Key Request reply, encryption, L2CAP PSM
   0x11/0x13 channels, and interrupt-channel reports.
3. **USB host (Camera Connection Kit).** The A4 has a separate host complex. In
   the DT, `usb-complex` (ctl 0xbf108000) has these children: `usb-device` (OTG,
   +0x0, IRQ 0x0d), `usb-ehci` (+0x300000 → **0x86400000, IRQ 0x0e**),
   `usb-ohci0` (0x86500000, IRQ 0x0f), and `usb-ohci1` (0x86600000, IRQ 0x10).
   The kernel includes `AppleSynopsysUSBEHCI` (matches `usb-ehci`/`usb-synopsys_ehci`),
   `AppleUSBOHCI`, `IOUSBHIDDriver`, and `AppleUSBHIDKeyboard`. `AppleEmbeddedUSBArbitrator`
   picks device or host from the PMU cable-type notification ("Connected to a
   USB Device") or from the DT property `"force-usb-host"` ("triggering host
   mode"). Host and device share the one PHY, so while the guest is in host
   mode, usbmux and USB Ethernet are gone.

A second path exists for plain HID. `IOHIDResource` (`IOHIDResourceDeviceUserClient`,
`IOHIDUserDevice`) is in the kernel, so any root process can create a real
`IOHIDDevice` from a report descriptor. That is exactly what iapd and BTServer
do after their transport work.

### Recommended: guest `hidbridge` over usbmux

- Guest: a small daemon (roughly 150 lines of C) started by launchd from the
  NAND image. It listens on a TCP port, calls `IOHIDUserDeviceCreate` with a
  standard boot-keyboard descriptor (VID 0x5ac lets `AppleHIDKeyboardEmbedded`'s
  Fn map apply, but that's optional), and passes each 8-byte report it
  receives to `IOHIDUserDeviceHandleReport`. It needs unsigned code, which is
  already covered by gap-unsigned-guest-code-3.2.md (debug-enabled + AMFI
  boot-args, or an ldid pseudo-signature).
- Host: the Mac app maps `NSEvent` key codes to HID usages and writes reports
  to that port through usbmuxd (`usbmuxd_connect`). This is the same plumbing
  the app already uses for lockdown.
- Result: the keyboard goes through IOHIDFamily → UIKit's hardware-keyboard
  path (`-[UIApplication setHardwareKeyboardLayoutName:]`), the same route a BT
  keyboard takes, and usbmux keeps working. It's not tied to a transport, so
  moving later to BT or USB host doesn't change the host side.
- Cost: 1-2 days, most of it spent on the NAND-image plumbing and the key
  map. Risk: the path from IOHIDUserDevice to a hardware keyboard should work,
  because BTServer uses it for BT keyboards on this exact build. It hasn't been
  run.

### Implemented: `contrib/ipad1-hidbridge`

- `hidbridge.c` builds with `build.sh` to `hidbridge.dylib` (armv7, `-marm`, ldid-signed). It is a
  dylib, not an executable, because 3.2's dyld can't load LC_MAIN. Its constructor never returns.
  `com.qemu.hidbridge.plist` runs `/bin/launchctl` with
  `DYLD_INSERT_LIBRARIES=/usr/local/lib/hidbridge.dylib`, so launchctl's own `main` never runs. The
  log goes to `/var/log/hidbridge.log`, and setting `HIDBRIDGE_TRACE=1` in the plist logs every report.
- Images: `ipad1_rootfs.py build --hidbridge` installs both files, owned by root. The dylib is
  ad-hoc signed, so boot with the AMFI boot-args.
- Host: `hidkbd.py type TEXT` or `hidkbd.py keys <kVK codes>`. It goes through usbmuxd by default, or
  `--tcp HOST:PORT` connects to a forwarded port. `MAC_TO_HID` in that file is the macOS
  virtual-keycode to HID-usage table that the app should reuse.

**Wire format.** The guest listens on TCP port **5213** and takes one client at a time. With usbmuxd,
use `Connect` with `PortNumber` = htons(5213). The stream is a sequence of 8-byte USB HID boot-keyboard
input reports with no header or framing:

| byte | meaning |
|---|---|
| 0 | modifier bits: 0x01 LCtrl, 0x02 LShift, 0x04 LAlt, 0x08 LGUI (Cmd), 0x10-0x80 the right-hand ones |
| 1 | reserved, 0 |
| 2-7 | up to six pressed key usages (HID page 7), 0 = empty |

Each report is the complete key state. To press a key, send a report that contains it; to release
it, send one that doesn't. The guest sends nothing back. When the client disconnects, the guest
injects an all-zero report, so no key stays stuck.

**Verified on the real 7B500 iPad:** the device appears as IOHIDUserDevice, then IOHIDInterface, then
AppleHIDKeyboardEventDriver, with SpringBoard's IOHIDEventServiceUserClient attached, and reports
sent through macOS usbmuxd return kIOReturnSuccess.

### Fallback: QEMU's EHCI + `usb-kbd`

Map `hcd-ehci-sysbus` at 0x86400000 (IRQ 0x0e), plus `hcd-ohci-sysbus`
companions if the driver insists on them, and attach `-device usb-kbd`. Set
`force-usb-host` in the DT we build, on whichever node the arbitrator reads.
Host keystrokes then arrive through QEMU's input layer. Risks:
`AppleSynopsysUSBEHCI` may touch Synopsys INSNREG registers that QEMU's EHCI
doesn't model, the clock/phy writes to `usb-complex` go to an unmapped block,
and **the guest loses usbmux while in host mode**. Use this only if the
guest-helper route fails.

## 2. USB Ethernet

### What the guest exposes

In the real device's log, the fourth configuration is "PTP + Apple Mobile
Device + Apple USB Ethernet" (interfaces PTP, AppleUSBMux, AppleUSBEthernet).
`AppleUSBEthernetDevice.kext` 2.0.1 matches `IOUSBDeviceInterface` with
`USBDeviceFunction=AppleUSBEthernet`. It is registered unconditionally at boot
(`gated_registerFunction Register function AppleUSBEthernet`), so nothing needs
enabling on the guest side. The host just has to select that configuration.
From the kext's strings:

- Alt setting 0 has no endpoints (`addAltSetting 0`), and alt setting 1 has
  bulk IN and bulk OUT (`createPipe failed for Bulk IN/OUT`). The host must
  send `SET_INTERFACE alt 1` (`kIOUSBDeviceInterfaceMessageTypeSetAlternateSetting`)
  before any traffic flows.
- **Protocol: one raw Ethernet frame per bulk transfer, with a ZLP when the
  length is a multiple of the max packet size** (`ZLP failed`,
  `mbuf length > %d, dropping packet`). There's no CDC/NCM framing. This is
  Apple's proprietary class 0xFF / subclass 0xFD, the one macOS's
  `AppleUSBEthernetHost` binds for iPhone tethering. Read the exact class bytes
  from the config-4 descriptor at runtime rather than trusting this.
- Link status: `Link Status - %d` / `failed to allocate Link Status MD`. This
  is a status buffer the host reads, probably a vendor control request. Find
  out which by tracing `IT_USB_TCP_DEBUG=1`. The interface may stay link-down
  until the host sends it.
- MACs: the device MAC is fixed at 0a:0b:ad:0b:ab:e0. The host MAC is taken
  from the `sdio` node's `local-mac-address` (the Wi-Fi MAC); if that's missing,
  the kext makes up a fake one.

### Why it doesn't work today

`usb-qemu.c:690` stops at the **first** configuration that contains the mux
interface, which is configuration 3 ("PTP + Apple Mobile Device"). It never
sends `SET_CONFIGURATION 4`, so the Ethernet function never starts. The macOS
kext can't help either, because the device isn't on a real USB bus. Everything
has to happen in userland.

### Recommended implementation

1. `usb-qemu.c`: prefer the configuration that has both the mux interface and
   the Ethernet interface (class 0xFF / subclass 0xFD), and fall back to the
   mux-only one. After `SET_CONFIGURATION`, send `SET_INTERFACE(eth, 1)`.
   Add the Ethernet bulk IN to the poll loop that already exists for the mux
   endpoints. The NAK-until-armed flow control carries over unchanged.
2. Link it against **libslirp**. It's already installed with Homebrew
   (4.9.4, a QEMU dependency) and needs no root or vmnet entitlement. Each bulk
   IN frame goes to `slirp_input`, and slirp's `send_packet` callback sends a
   bulk OUT with a ZLP. This gives the guest a DHCP server, DNS, and NAT to
   the Mac's network with no host configuration. Later, forward selected host
   ports with `slirp_add_hostfwd`.
3. Guest: configd/IPConfiguration only brings up interfaces that have a
   network service. Add a DHCP service for the USB Ethernet interface
   (en1, or whatever `ifconfig` shows) to
   `/Library/Preferences/SystemConfiguration/preferences.plist` in the NAND
   image. Check whether 3.2's SCNetworkReachability counts a non-Wi-Fi,
   non-pdp_ip default route as reachable. Safari and CFNetwork should work, but
   apps that insist on `kSCNetworkReachabilityFlagsIsWWAN` or Wi-Fi may refuse.

Cost: 3-5 days, mostly the descriptor walk, link-status discovery, and the
guest preferences. Throughput is limited by the TCP transport's round trip on
each transfer, which is fine for browsing.

If QEMU's `-netdev` ever becomes preferable (for example
`vmnet-shared`), step 2 changes to shuttling frames over a socket to QEMU.
Nothing else changes.

## 3. Wi-Fi (BCM4329)

What the guest would need:

- Transport: `AppleS5L8920XIOPSDIO` sends SDIO commands through the IOP ring
  (`RING_SDIO 3`). `s5l8930_iop.c` answers only `SDIO_OP_PING` and returns
  `SDIO_STATUS_UNKNOWN` for everything else. We'd have to reverse the
  CMD52/CMD53 opcodes in the IOP firmware's SDIO task (fw 0x1524) and model
  them, including the scatter DMA.
- A fake BCM4329 behind it: F0 CCCR/CIS tuples (`IOSDIOManufacturerTuple`
  matching), F1 backplane window and SBSDIO clock CSR, chipcommon ID,
  accepting the RAM firmware download and NVRAM, and then the "firmware
  ready" handshake. `wifiFirmwareLoader` gets the image through the driver's
  `firmwareData` path. `/usr/share/firmware/wifi` isn't in the mounted rootfs,
  so find where it actually lives before starting.
- Then the dongle protocol that AppleBCMWLAN-2.60 speaks: SDPCM framing, CDC
  ioctls (dozens of `WLC_*` get/set calls plus iovars like `cur_etheraddr`,
  `ver`, glomming), async events (`WLC_E_*`) for scan results, join, and
  link, and the in-dongle supplicant events (`handleSupplicantEvent`) for
  WPA2. On top of that, a fake SSID and a data path to slirp.

This is 3-6 weeks of reverse engineering with no reference implementation
(openiBoot's `wlan.c` is S5L8900-only and stops after the firmware upload).
The only thing it adds over USB Ethernet is that apps see Wi-Fi reachability.
**Recommendation: don't build it.** If Wi-Fi reachability turns out to
matter, get it more cheaply by answering the SDIO ring with "no card" so
AppleBCMWLAN fails cleanly, and have the guest treat the USB Ethernet link as
its primary route.
