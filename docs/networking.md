# Networking on the emulated iPod touch 2G

Where IP connectivity could come from, and what the hardware and the firmware
actually allow. Written against iPhone OS 2.1.1 / build 5F138, the image this
tree boots.

## Summary

There is no Ethernet MAC in the S5L8720, so QEMU's `hw/net/*` models and
`-netdev` have nothing to attach to. That leaves three candidate paths, and
only one of them is real:

| Route | Verdict |
| --- | --- |
| USB ethernet function (CDC ECM/NCM, RNDIS, tethering) | **Ruled out.** The firmware ships no such function and no driver that could back one. Evidence below. |
| cp15 guest-services socket shim (`hw/arm/guest-services.c`) | Dormant. Needs guest code to issue the hypercall, and is layer 4 only - no interface, so nothing in iOS routes through it. |
| PPP over the serial multiplexer | Real mechanism, wrong device. See below. |
| BCM4325 SDIO WiFi (`hw/arm/ipod_touch_sdio.c`) | **Works.** Behind `wifi=on`, iOS takes a DHCP lease, ARPs, and serves TCP over the emulated dongle. See "The device is on the network" below. |

### PPP is a second interface-capable path, and it needs a baseband

"Only the WiFi driver can produce an interface" is too strong, and a search for
`IONetworkingFamily` clients structurally cannot see why: PPP creates
interfaces through the BSD network stack, not through IOKit driver matching.
The image ships `com.apple.nke.ppp` and `com.apple.driver.AppleSerialMultiplexer`,
whose source paths include `MuxNetworkInterface.cpp` and `MuxNetworkPolicy.cpp`
with RS232, SPI and H5 adapters. iOS 2.x can bring a network interface up over
a serial link.

It is the cellular data stack - PPP and PDP to the baseband over the serial mux -
and iPod2,1 has no baseband. The reason this route is unusable here is "no
baseband on this device", not "no such mechanism exists".

## Why USB ethernet is ruled out

Two independent lines of evidence, one from the firmware image and one from the
running device.

### The kernelcache contains no USB network driver

`IONetworkingFamily` is present in the 5F138 kernelcache, but only two kexts
link against it, and both are WiFi:

```
com.apple.driver.AppleBCM4325     (BCM4325 SDIO, this device)
com.apple.driver.AppleMRVL868x    (Marvell, other devices)
```

There is no `AppleUSBEthernetHost`, no CDC ECM/NCM driver, and no RNDIS driver.
USB tethering did not ship until iOS 3.0.

`com.apple.driver.AppleTetheredDevice` looks promising by name and is not
related: it matches `IOProviderClass = AppleARMIICDevice` with
`IONameMatch = "tethered,tethereddevice"`, an I2C node, and does not link
`IONetworkingFamily` at all.

### The running device exposes no communications-class interface

`contrib/ipod-touch-usbdesc.py` walks the complete descriptor set over the
tcp_usb transport. Against a booted 2.1.1 device, every configuration:

```
DEVICE: USB 0x0200 idVendor 0x05ac idProduct 0x1293 bNumConfigurations 3
        manufacturer 'Apple Inc.'  product 'iPod'

configuration 1  "PTP"                        1 interface
configuration 2  "iPod USB Interface"         3 interfaces
    interface 0        class 0x01 audio, subclass 0x01
    interface 1 alt 1  class 0x01 audio, subclass 0x02   ep 0x81 isochronous
    interface 2        class 0x03 HID                    ep 0x82 interrupt
configuration 3  "PTP + Apple Mobile Device"  2 interfaces
    interface 1        class 0xff subclass 0xfe protocol 0x02
                       ep 0x01 bulk OUT 512, ep 0x81 bulk IN 512
```

No interface has `bInterfaceClass` 0x02 (communications) or 0x0a (CDC data),
there is no IAD, and no CDC ethernet networking functional descriptor
(`CS_INTERFACE` subtype 0x0f) appears anywhere. Configuration 3's interface 1 is
the usbmux endpoint pair, which is what `usbmuxd` already uses.

Reproduce with:

```
python3 contrib/ipod-touch-usbdesc.py 1330 105
qemu-system-arm -M iPod-Touch,...,usb-attached=on,usb-patch-mux-gate=on,\
    usb-tcp-addr=127.0.0.1:1330 ...
```

## What the WiFi route requires

`hw/arm/ipod_touch_sdio.c` began as a probe stub: a faked CIS, faked
clocks-ready, and a data read that returned a 4-byte header and no payload.
`sdio_exec_cmd` had the CMD5 response commented out, so the card never
announced itself and `AppleS5L8900XSDIO::enumerateCards` could not succeed.

`AppleBCM4325` is a Broadcom dongle driver of the same shape as Linux's
`brcmfmac`. Its strings show the full bring-up it expects:

- read the chip ID and enumerate backplane cores over function 1
- read OTP for the MAC address (`Error, unable to obtain MAC address, can't
  proceed any further`), with static vars from the kext personality as the
  fallback (`BCM4325Vars`, an nvram string carrying `boardtype=0x04c6`,
  `sdmaxblk2=512` and friends)
- download firmware and the vars block into dongle RAM, **reading both back to
  verify** (`Firmware write verfication failed, trying again`)
- start the dongle and speak SDPCM over function 2: a CDC control channel
  (`Broadcom SDPCMD CDC driver`, `Parsing the cdc header`) and a BDC data
  channel (`bdc->flags`, `bdc->priority`)
- drive association with the standard `WLC_*` ioctl set (`WLC_UP`, `WLC_SCAN`,
  `WLC_SET_SSID`, `WLC_SET_WSEC`, `WLC_GET_BSSID`, ...) and consume asynchronous
  events

None of that needs 802.11 to be simulated - association can simply be asserted -
but it does need the dongle side of the SDIO and SDPCM protocols to be real.

## Where the WiFi route stands

The stack starts on its own at boot - no user action, no Settings toggle. With
`boot-args=io=0x37` the matching is visible on the serial console.

On a stock build the conversation ends immediately:

```
AppleS5L8900XSDIO::start(sdio): SDIO Revision 8720X
IOSDIOController::enumerateSlot: Searching for SDIO device in slot: 0
SDIO CMD: 5 ... (x101)
IOSDIOController::enumerateSlot: Timed out waiting for card to become ready
AppleS5L8900XSDIO::enumerateCards: Unable to communicate with SDIO device
```

101 CMD5s and no answer, because the R4 response is commented out in the model.
Settings shows "No Wi-Fi" and the row is disabled, which is downstream of this
and not a separate problem.

With `wifi=on` the model answers CMD5 and presents a real CIA, and the chain
runs all the way to the driver:

```
IOSDIOIoCardDevice::parseCIS: Device manufacturer Id(4d50), Product Id(4d48)
AppleBCM4325::start(IOSDIOIoCardDevice)
AppleBCM4325::initHardware(): BCM4325 revision D0
```

after which it programs the backplane window and writes roughly 200 KB of
firmware over function 1. With the mailbox and SDPCM framing in place it then
reaches the control channel:

```
[SDIO] 192 KiB written to the backplane (now at 0x0002f000)
[SDIO] dongle announced ready
[SDIO] first SDPCM frame on function 2 (512 bytes)
[SDIO] CDC command 84 (get), 4 bytes, flags 0x00000000
```

Command 84 is `WLC_SET_COUNTRY`, which is exactly what `initDongle` issues
first. The upper half of the flags word is the transaction id and increments on
each retry, confirming the CDC header layout.

The reply is collected and accepted - `WLC_SET_COUNTRY` succeeds, with no
"Failure to set country code" anywhere in the run. Getting there needed three
things that are worth writing down, because each looked like a hang:

- **`AppleS5L8900XSDIO`'s interrupt register accumulates two bits.** Its handler
  (`0xc061ba88`) reads the status, masks it against the enabled set, writes the
  value straight back to clear, then dispatches bit 0 as transfer complete and
  bit 1 as the card's own interrupt. Assigning bit 0 at the end of a transfer
  wiped out a bit 1 raised earlier in the same transaction.
- **CCCR `INT_PENDING` (0x05) has to be answered.** The driver reads it to find
  which function raised the card interrupt. Answering zero means it concludes
  nothing is interrupting and waits out its timeout without ever looking at
  function 2. Deriving it from the dongle's masked `intstatus` also makes it
  clear itself when the mailbox is acknowledged.
- **`I_HMB_FRAME_IND` has to be cleared when the queue drains.** Left set, the
  driver reads again, gets a zero-length frame, hands it to its command manager
  and logs "there is no pending command" - forever, at tens of megabytes of
  serial output per minute.

- **"No more frames" is an all-zero tag.** A well-formed tag claiming a length
  of zero is a frame as far as this driver is concerned; it reads the body,
  finds nothing, and complains.

The collection sequence itself is: read `tohostmailboxdata` (function 1,
`0x0a04c`, which is `0x18002_04c` with the 32-bit access flag), clear
`intstatus` (`0x0a020`), then read function 2 twice - twelve bytes for the
header, then the body. A frame therefore has to survive being read in pieces.

- **The CDC header here is twelve bytes, not sixteen** - command, length,
  flags, and no status word. A `WLC_UP` request is a 24 byte frame: twelve of
  SDPCM and twelve of CDC with no payload. Assuming sixteen makes every
  payload-free request shorter than the header, so it is dropped without a
  reply. It also throws off the reply's length field, which
  `AppleBCM4325CmdManager.cpp:445` asserts must equal the length the command
  expects.

### Where it gets to

The driver now runs its whole initialisation against the model. Answering every
control command with success and a zeroed payload is enough:

```
CDC 84  WLC_SET_COUNTRY   CDC 262 get_var (27)   CDC 86  (4)
CDC 2   WLC_UP            CDC 263 set_var (22)   CDC 263 set_var (12)
CDC 262 get_var (260)     CDC 83  (4)
CDC 38  set (4)           CDC 263 set_var (27)

AppleBCM4325::initFirmware(): successful initialization
IO80211Interface::attach(AppleBCM4325)
IONetworkStack::attach(IO80211Interface)
AppleBCM4325: Ethernet address 00:23:32:6e:aa:10
AppleBCM4325::setPowerStateGated() : Powering On
```

**An `IO80211Interface` is attached to the network stack with the right MAC.**
That is the first network interface this emulator has ever had, and iOS agrees:
Settings' Wi-Fi row goes from a greyed-out "No Wi-Fi" to "Not Connected", and
the Wi-Fi Networks pane opens with the toggle ON and "Choose a Network..."
above an empty list.

### The control commands the driver actually issues

Logged with their iovar names, which is the only way to tell two `WLC_SET_VAR`s
apart. In order, from `initDongle` to the first scan:

```
84  WLC_SET_COUNTRY          263 set_var  sup_wpa
2   WLC_UP                   263 set_var  allmulti
262 get_var  ver             263 set_var  mcast_list
83  WLC_GET_COUNTRY          262 get_var  qtxpower
38  set (4 bytes)            263 set_var  deepsleep   (repeatedly)
263 set_var  event_msgs      263 set_var  iscan       <- tapping into Wi-Fi
262 get_var  event_msgs
263 set_var  scan_passive_time
86  get (4 bytes)
```

Two things fall out of this:

- `get_var ver` is answered with a zeroed payload, so the driver logs an empty
  `BCM4325 Firmware Version:`. Harmless now, worth filling in later.
- **Scanning goes through the `iscan` iovar**, the older incremental-scan
  interface, not `escan`. Tapping into the Wi-Fi pane sends a 206-byte
  `set_var iscan`. The list stays empty because nothing answers it.

### What the driver is blocked on

Characterised directly. With `set_var iscan` acknowledged as a success, the
driver **never asks for results**. Over ninety seconds in the Wi-Fi pane it
issued `set_var iscan` six times - roughly every fifteen seconds - and zero
requests for `iscanresults`, `WLC_SCAN_RESULTS` or anything else that would
carry a BSS list:

```
263 set_var iscan     (206 bytes)
263 set_var deepsleep
263 set_var deepsleep
263 set_var iscan     (206 bytes)      <- 15s later, same thing again
...
```

So it is **waiting for an asynchronous event, not polling**. That event is now
delivered, and the driver responds to it - see below.

### Delivering events

Two details here contradict what `brcmfmac` would lead you to write, and both
fail silently:

- **Events do not go on the event channel.** The receive dispatch answers
  channel 1 with "WTF?? Got an event packet!!!" and drops it. Events arrive on
  the **data channel** as an ordinary 802.3 frame that `handleDataPacket`
  recognises by its ethertype.
- **The BDC header is six bytes, not four.** `handleDataPacket` logs byte 0 as
  `bdc->flags` and byte 1 as `bdc->priority`, then advances the packet by six
  before treating the rest as ethernet - the two extra bytes are padding that
  lands the IP header on a four-byte boundary. Get this wrong and the entire
  frame is shifted by two: the driver reads it, the OUI check fails, and it
  discards it **without logging anything at all**.

The rest is pinned by `handleEventPacket`, which memcmps three bytes at packet
offset 0x13 against `00:10:18` and requires a big-endian `usr_subtype` of 1 at
0x16, with the `wl_event_msg_t` at 0x18, all big-endian. Event numbers come
from the driver's own dispatch table at `0xc0330e18` - 49 entries, of which 0,
3, 6, 7, 9, 12, 16, 17, 19, 26, 45 and 48 have real handlers. That confirms the
standard numbering, and `WLC_E_SCAN_COMPLETE = 26`.

With `WLC_E_SCAN_COMPLETE` sent after the scan request is acknowledged, the
driver stops waiting and asks for results:

```
CDC 263 set_var iscan (206 bytes)
[SDIO] sending event 26, status 0 (76 byte frame)
CDC 262 get_var iscanresults (2024 bytes)      <- never happened before
```

The handler for event 26 is a single call on the object at `self+0x2b4`, the
scan manager - it takes no parameters from the event at all.

### Where it stops

**The network list still shows a spinner.** `get_var iscanresults` is answered
with 2024 zero bytes, and the scan manager does not accept that as a final
answer: it re-arms the scan every fifteen seconds and logs
`IO80211ScanManager::startScan: Timing out scan requested: 110, and now: 120!`.

A zeroed reply decodes as `wl_iscan_results_t` with `status` 0
(`WL_SCAN_RESULTS_SUCCESS`) but also `version` 0, `buflen` 0 and `count` 0.
The next step is to fill that structure in properly - the version constant this
build expects, a consistent `buflen`, and at least one `wl_bss_info_t` - which
is where the real 802.11 data structures start and where this stopped.

### A second, independent gap: deep sleep

Once the display dims, `configd` logs "WiFi: Display off. Adjusting scan
intervals for dim screen" and the driver tries to put the dongle to sleep. That
path fails and retries forever:

```
AppleBCM4325DeviceInterfaceSdio::txPacket(): Error sending packet to SDIO layer : I/O timeout
AppleBCM4325CmdManager::processPendingList(): Failure to send command to device: I/O timeout
AppleBCM4325::enterDeepSleep(): Unable to enter deep sleep: I/O timeout
```

Note this is a **send** failure, not a missing reply - the `set_var deepsleep`
before it is received and acknowledged normally. Something about the model's
state after a deepsleep request stops the next frame being accepted. Worth
looking at whichever of `CHIPCLKCSR` or the frame control register the driver
touches on the way into sleep, since the model answers `CHIPCLKCSR` with a
constant "ALP and HT available" regardless of what was requested.

It does not survive. Seconds later the guest panics:

```
kernel abort type 4: fault_type=0x1, fault_addr=0x38
pc: 0xc03438f0  lr: 0xc0332508
```

`0xc0332498` is the deep sleep timer handler; it logs "deep sleep timer" and
then calls `0xc03438f0`, a four instruction "are we associated" accessor that
loads from `+0x38` of the object at `self+0x2bc`. That object is null, so the
fault address is 0x38 exactly.

**That crash is an artifact of `boot-args=io=0x37`, not a protocol gap.** With
IOKit matching logs on, the firmware download runs at about 256 seconds per
64 KiB; without them the whole download finishes in under a second. Measured
with the same binary, changing only the boot argument:

```
io=0x37     64 KiB: 8435 register accesses      128 KiB: 256.0s, 664 accesses
no args     64 KiB: 8435 register accesses      128 KiB:   0.0s, 664 accesses
```

The emulated device does the same work either way - 664 register accesses for
64 KiB - so the cost is entirely inside the guest. Stretched over twenty
minutes, `AppleBCM4325::start()` loses a race with one of its own timers: the
deep sleep handler fires while `start()` is still running and dereferences
state `start()` has not assigned yet. Run without `io=0x37` and the whole
bring-up takes a couple of minutes and that crash does not happen.

The only panic left on a `wifi=on` boot is the pre-existing
`AppleMPVDDriver::setPowerStateGated` idle-sleep one (`fault_addr=0xec3fd01c`),
which is unrelated to WiFi and fixed on another branch.

**So: use `wifi=on` on its own. Add `io=0x37` only when you need matching logs,
and expect it to cost roughly 250x on this path.**

Confirmed on a clean run: `wifi=on` with no boot arguments reaches
`initFirmware(): successful initialization` and publishes the interface about
thirty seconds into the boot, with zero occurrences of `fault_addr=0x38`.
`start()` completes. The next thing to build is stage 4 - accept `set_var
iscan`, push a scan-complete event on channel 1, and answer `get_var
iscanresults` with a synthetic BSS.

## The device is on the network

Scan results and a real association turned out not to be on the critical path.
The driver believes whatever its own event dispatch table is told, so pushing
`WLC_E_AUTH`, `WLC_E_ASSOC`, `WLC_E_SET_SSID` and a `WLC_E_LINK` carrying the
link flag brings the carrier up directly:

```
AppleBCM4325 Joined BSS: BSSID = 02:00:5e:10:00:01, rssi = -45, ...
AirPort: Link Up on en0
```

iOS then does the rest with no help at all. `SDPCM` channel 2 is bridged to a
`NetClientState`, slirp is re-enabled in the build, and the capture on the
backend shows a complete bring-up:

```
IP 0.0.0.0.68 > 255.255.255.255.67: BOOTP/DHCP, Request from 00:23:32:6e:aa:10
IP 10.0.2.2.67 > 255.255.255.255.68: BOOTP/DHCP, Reply, length 548
ARP, Probe 10.0.2.15          <- duplicate address detection
ARP, Announcement 10.0.2.15
ARP, Request who-has 10.0.2.2 tell 10.0.2.15
ARP, Reply 10.0.2.2 is-at 52:55:0a:00:02:02
```

and the guest's TCP stack answers on the address it was given. Forwarding a
host port to lockdownd, which listens on 62078 on every interface:

```
IP 10.0.2.2.64319  > 10.0.2.15.62078: Flags [S],  seq 1984001
IP 10.0.2.15.62078 > 10.0.2.2.64319:  Flags [S.], seq 954588535, ack 1984002
IP 10.0.2.2.64319  > 10.0.2.15.62078: Flags [P.], seq 1:241, length 240
IP 10.0.2.15.62078 > 10.0.2.2.64319:  Flags [.],  ack 241
IP 10.0.2.15.62078 > 10.0.2.2.64319:  Flags [F.]
```

Three way handshake, 240 bytes of request delivered and acknowledged by the
guest, clean shutdown. lockdownd hangs up because it will not serve an
unpaired peer over WiFi, which is policy, not transport.

Run it with:

```
qemu-system-arm -M iPod-Touch,...,wifi=on \
    -netdev user,id=wifi0,net=10.0.2.0/24,host=10.0.2.2,dhcpstart=10.0.2.15 \
    -object filter-dump,id=cap0,netdev=wifi0,file=wifi.pcap
IPOD_WIFI_FAKE_LINK=40 ...
```

`IPOD_WIFI_FAKE_LINK` is the number of seconds after `WLC_UP` to assert the
association. It is behind an environment variable because it is a claim about
state the model has not really reached.

> **slirp is now required, so the configure flags changed.** This tree used to
> be configured with `--disable-slirp`; the network backend needs it, so it is
> `--enable-slirp` now (libslirp comes from Homebrew). Meson does **not** pick
> this up on its own - an existing `build/` directory will keep building
> happily with slirp off and `-netdev user` will simply not exist, which looks
> like the WiFi model failing rather than a stale build. Re-run `configure` in
> the build directory after pulling this.

### Two things that had to be fixed first

- **A get's reply has to be as long as the buffer the driver offered**, not as
  long as the request. The model clamped both, so a 1148 byte
  `WLC_GET_BSS_INFO` came back with nothing, tripped
  `AppleBCM4325CmdManager.cpp:213`, and left the driver reading its own
  uninitialised buffer - which is where the garbage BSSID in the join line came
  from. The same clamp was cutting `get_var iscanresults` from 2024 bytes down
  to 13, so the scan results path was never being answered either.
- **`WLC_GET_RSSI` (127) and `WLC_GET_RATE` (12) are polled** to drive the
  status bar. Answering zero is why the WiFi icon showed no bars.

### Safari renders a page

Two things in the **filesystem**, not the emulator, stood between the working
transport and a browser that would use it. Both are properties of this image.

### The image has no SystemConfiguration preferences

`/var/preferences` is an empty directory, `/Library/Preferences/SystemConfiguration`
is empty, and there is no `preferences.plist` anywhere on the volume - this
device was never taken through joining a network, which is what would have
written one. So `PreferencesMonitor` published no service, `IPMonitor` never
elected a primary, and `State:/Network/Global/IPv4` was never set. DHCP still
ran, because IPConfiguration is driven by the link event rather than by a
service, so the device had an address and no idea it was on a network.

Writing a minimal service - one set, one service, `ConfigMethod = DHCP`,
`Hardware = AirPort`, `DeviceName = en0`, and a `__LINK__` from the set to the
service - is enough. The classic SCPreferences layout is what 2.x parses; the
schema only drifted later. With it in place, and configd made verbose
(`-v -V com.apple.SystemConfiguration.IPMonitor` in its LaunchDaemon):

```
IPMonitor: service_order <array> { 0 : 0 }
IPMonitor: serviceID 0 changed IPv4 dictionary
IPMonitor: IPv4 service election
IPMonitor: 0 is the new primary IPv4
IPMonitor: IPv4 route add default 10.0.2.2 interface en0 direct 0
IPMonitor: 0 is the new primary DNS
  State:/Network/Global/IPv4 : { Router : 10.0.2.2, PrimaryInterface : en0,
                                 PrimaryService : 0 }
  State:/Network/Global/DNS  : { ServerAddresses : { 0 : 10.0.2.3 } }
```

and Aeropuerto agrees: `WiFi: Already connected to qemu-ios.`

### A page, in Safari

With a host page behind `guestfwd=tcp:10.0.2.100:80-tcp:127.0.0.1:27303`,
typing `10.0.2.100` into Safari renders it. The whole exchange, guest first:

```
ARP, Request who-has 10.0.2.100 tell 10.0.2.15
ARP, Reply 10.0.2.100 is-at 52:55:0a:00:02:64
IP 10.0.2.15.49152 > 10.0.2.100.80: Flags [S], seq 2113151649
IP 10.0.2.100.80 > 10.0.2.15.49152: Flags [S.], ack 2113151650
IP 10.0.2.15.49152 > 10.0.2.100.80: length 384: HTTP: GET / HTTP/1.1
IP 10.0.2.100.80 > 10.0.2.15.49152: length 186: HTTP: HTTP/1.0 200 OK
IP 10.0.2.100.80 > 10.0.2.15.49152: length 204: HTTP
IP 10.0.2.15.49152 > 10.0.2.100.80: Flags [F.]
```

with `"GET / HTTP/1.1" 200` in the host server's log at the same second, and the
page's title and body text on the screen. **iPhone OS 2.1.1 is browsing over
emulated WiFi.**

### Names do not resolve yet: nothing runs mDNSResponder

Numeric URLs work end to end; hostname URLs still fail with "not connected to
the Internet" and put **nothing** on the wire, not even a DNS query. The reason
is not reachability - the global state above is published and an address-based
load works from the same Safari session.

`/usr/sbin/mDNSResponder` is on the image, but
`/System/Library/LaunchDaemons` contains only eleven plists and none of them is
`com.apple.mDNSResponder.plist`. It never logs a line. Every name lookup on
Darwin goes through it, so `SCNetworkReachabilityCreateWithName` and `CFHost`
fail before a packet is sent, which is exactly the observed shape.

Adding a LaunchDaemon for it is the next thing to try.

### Where the SSID and channel actually come from

They come from the **information elements** appended after `wl_bss_info_t`, not
from its fixed `SSID`/`channel` fields. Filling only the fixed fields left
`ssid = ""` and `channel = 0` while BSSID and RSSI - read from the same
structure - were correct. Appending an SSID IE, a supported-rates IE and a DS
parameter set fixed both.

Also worth knowing: the stock image's `/etc/resolv.conf` is the **original
owner's**, listing an IPv6 nameserver first. Weather failed with no packets on
the wire because of it. It is now overwritten offline in the working NAND.

### The MAC address is a CIS tuple

`AppleBCM4325::processConfigData` asks its interface layer for "OTP" data and
parses it as a tuple chain. For the SDIO interface that data is the card's CIS,
so the MAC has to be a tuple:

```
0x22 0x08 0x04 0x06 <six address bytes>      CISTPL_FUNCE, extension type 4
```

The parser (`0xc032f8e0` in the 5F138 kernelcache) walks the chain, stops at
`0xFF`, and for a `0x22` tuple requires the first body byte to be 4 and the
second to be 6 before copying six bytes. It also recognises a `0x80` vendor
tuple with subtype `0x81`, which is where Apple's config blob - BT address,
WiFi calibration - would live.

If nothing matches, the address stays zero, `processConfigData` compares it
against an all-zero constant at `0xc034cbe0` and gives up with "unable to obtain
MAC address, can't proceed any further".

### What is still missing, and what it costs

The prior estimate for this route was "multi-month, high-risk". Having the
driver attach and start downloading firmware within an evening argues for
something smaller. The remaining work, in the order it has to be done, each
stage verifiable by the driver's own log:

1. ~~**Dongle bring-up handshake.**~~ Done. The SDIOD core sits at backplane
   `0x18002000`; the model answers with a firmware-ready mailbox message and an
   interrupt when the driver starts the core.
2. ~~**SDPCM framing on function 2.**~~ Done. Four-byte hardware tag (length and
   its complement), eight-byte software header (sequence, channel, next length,
   header length, flow control, credit). Channels 0 control, 1 event, 2 data.
3. ~~**The CDC control channel.**~~ Done well enough for initialisation:
   everything is acknowledged with success and a zeroed payload, which the
   driver accepts. Returning real values will matter once association is real.
4. **Events and a fake association.** The next thing to build, and the thing
   that stops the deep sleep panic. Push `WLC_E_*` on channel 1 and answer
   `WLC_SCAN` with a beacon list, so an SSID appears in Settings and the driver
   believes it joined. No 802.11 needs simulating.
5. **The data path.** BDC header plus 802.3 frames on channel 2, bridged to a
   `NetClientState` so the usual QEMU backends apply. `CONFIG_VMNET` is
   compiled in and slirp is explicitly disabled, so vmnet is the one to target;
   DHCP and NAT then come from the host.

### The bring-up sequence, from the driver

Addresses are from the 5F138 kernelcache; the emulator boots that exact build,
so they can be used directly.

`downloadFirmwareGated` (`0xc0335918`), after the image is in RAM:

- writes four bytes to backplane `0x18000634` (chipcommon), waits 10 ms,
  then calls an interface method that starts the core
- calls `initHardware` (`0xc0334b48`)
- calls `initDongle` (`0xc03335ac`)

`initHardware` finishes by:

- writing `0xe0` to backplane **`0x18002024`** - the SDIOD core is at
  `0x18002000` and this is its `hostintmask`. `0xe0` is the host mailbox set:
  flow-control state, flow-control change, frame indication. Failure is logged
  as "Failure to write interrupt mask register".
- writing four bytes to function 1 register **`0x1000d`**, the frame control
  register
- one more interface call that must return non-zero, or "Call to hardware init
  failed, exiting"

`initDongle` then immediately issues **`WLC_SET_COUNTRY` (ioctl 0x54)** with the
four bytes `"XX"` through `wlc_ioctl` (`0xc033b0a0`), retrying once on failure
with "Failure to set country code, trying again". So the very first thing the
dongle is asked after reset is a CDC control transaction - there is no simpler
intermediate milestone, and stages 2 and 3 have to land together.

`initFirmware` (`0xc0333eb4`) wraps the whole thing in five attempts with a
power cycle and a 100 ms delay between them, so a wedged bring-up looks like a
slow retry loop rather than a hard failure.

Revised estimate: one and a half to two and a half weeks of focused work. The
main risk left is that Apple's 2008 dongle protocol differs in detail from the
one `brcmfmac` documents - the driver itself warns about a protocol version
mismatch, so at least the version is negotiable and visible.
