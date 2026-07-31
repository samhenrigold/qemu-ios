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

after which it programs the backplane window and starts writing firmware over
function 1.

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

### What is still missing

Everything past firmware download: the dongle-side SDPCM/CDC/BDC protocol on
function 2, the `WLC_*` ioctl surface, asynchronous events, and a bridge to a
host network backend. `CONFIG_VMNET` is compiled in and slirp is explicitly
disabled, so vmnet is the intended backend.
