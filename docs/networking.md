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
| BCM4325 SDIO WiFi (`hw/arm/ipod_touch_sdio.c`) | The only path to a real network interface. Currently a probe stub. |

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

## What the WiFi route would require

`hw/arm/ipod_touch_sdio.c` is a probe stub: it fakes a CIS, reports clocks
ready, and answers a data read with a 4-byte length/checksum header and no
payload. Notably `sdio_exec_cmd` has the CMD5 response commented out, so the
card never announces itself and `AppleS5L8900XSDIO::enumerateCards` cannot
succeed.

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
firmware over function 1 - about twenty minutes of emulated time, so be patient
before calling a run wedged. With the mailbox and SDPCM framing in place it
then reaches the control channel:

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
That is the first network interface this emulator has ever had.

It does not survive. Seconds later the guest panics:

```
kernel abort type 4: fault_type=0x1, fault_addr=0x38
pc: 0xc03438f0  lr: 0xc0332508
```

`0xc0332498` is the deep sleep timer handler; it logs "deep sleep timer" and
then calls `0xc03438f0`, a four instruction "are we associated" accessor that
loads from `+0x38` of the object at `self+0x2bc`. That object is null, so the
fault address is 0x38 exactly.

This is the expected consequence of stopping halfway rather than a wrong turn:
nothing has told the driver it associated, so the state the timer expects was
never built. Stage 4 - events and a faked association - is what removes it, and
until then a run with `wifi=on` will panic shortly after the interface appears.
The boot is also much slower with `wifi=on`, since the firmware download alone
is about twenty minutes of emulated time.

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
