# The tcp_usb transport

How the emulated iPod touch 2G's USB device controller talks to a host bridge.

This is the **canonical specification**. Two implementations exist, in separate
repositories, and both are expected to track this document:

| Side | Implementation |
| --- | --- |
| Device | `hw/arm/ipod_touch_tcp_usb.c`, `hw/arm/ipod_touch_usb_otg.c` (this repo) |
| Host | `src/usb-qemu.c` in [samhenrigold/usbmuxd](https://github.com/samhenrigold/usbmuxd), branch `qemu-backend` |

The transport carries USB device-mode traffic over a TCP socket so that
`libimobiledevice` tooling can reach the emulated device. It originated on the
`dfu_s5l8720` branch of the upstream project and was restored and corrected.

## Roles and connection

**QEMU is the client and dials out; the host bridge listens.** The emulator
connects at device realize and retries on each core soft reset, so the bridge
may be started before or after the guest.

Configure the emulator with the `usb-tcp-addr=host:port` machine option. An
empty value disables the link entirely.

Note the guest needs roughly 100 seconds of boot before it has programmed the
USB core. Until then every transaction is answered with `NAK`, which is normal
and must be retried rather than treated as failure.

## Framing

Every exchange is a 5-byte packed header, optionally followed by a payload:

```c
struct tcp_usb_header {
    uint8_t  addr;    /* device address; see below */
    uint8_t  ep;      /* endpoint, USB_DIR_IN (0x80) set for IN */
    uint8_t  flags;
    int16_t  length;
} __attribute__((packed));
```

Flags:

| Bit | Name | Meaning |
| --- | --- | --- |
| `1 << 0` | `setup` | payload is an 8-byte SETUP packet |
| `1 << 1` | `reset` | signal USB reset to the guest |
| `1 << 2` | `enumdone` | signal enumeration complete |
| `1 << 3` | `hello` | version handshake (see below) |

The exchange is **strictly host-driven request/response**. The device never
initiates, so IN data only flows when the host polls for it.

- The host sends a header. For an OUT with `length > 0` the payload follows.
- The device replies with a header. For an IN with `length > 0` the payload
  follows.

`addr` in the reply is overwritten by the device with its own `DCFG` address.
That is how the host learns a `SET_ADDRESS` has taken effect.

## Negative lengths are status codes

A negative `length` in a reply is a QEMU `USB_RET_*` code, not a size:

| Value | Meaning | Host behaviour |
| --- | --- | --- |
| `-1` | `NODEV` | fatal |
| `-2` | `NAK` | **retry** |
| `-3` | `STALL` | endpoint refused the request |

**`NAK` is the entire flow-control mechanism.** Every endpoint NAKs until the
guest arms it. A host that treats a NAK as an error will not get past boot.

## Version handshake

The host's first exchange must be an **IN request on endpoint `0x7f`** with the
`hello` flag and `length >= 12`. The device replies with:

```c
struct tcp_usb_hello {
    uint32_t magic;            /* 0x42535554, "TUSB" */
    uint16_t version;          /* currently 1 */
    uint16_t reserved;
    uint32_t max_transaction;  /* largest payload accepted in one transaction */
} __attribute__((packed));
```

The host must refuse to proceed if the magic is wrong, the version differs, or
its own maximum packet size exceeds `max_transaction`.

A device that predates the handshake sees an endpoint outside its range and
answers `STALL`, so an old device fails loudly against a new host. A new device
is unaffected by a host that never asks.

### Why this exists

The two halves live in separate repositories and previously agreed on the
transport's constraints only by convention. They disagreed, and the failure was
silent rather than loud — see the size rules below. The handshake converts that
class of mistake into a startup error.

## Transaction size rules

These are the constraints that actually matter, and getting them wrong corrupts
data without any error being reported.

**A transaction cannot exceed `INT16_MAX` (32767) bytes**, because `length` is
an `int16_t`.

**The host must never split one logical packet across transactions.** This
transport is *transfer-oriented*, not packet-oriented: there are no ZLPs, so a
transaction that happens to be a multiple of the endpoint's max packet size has
no way to signal that it is the end. One transaction therefore means one
complete transfer, and the device retires the endpoint after each one. A
continuation lands in a transfer the guest already considers finished.

**The device accepts at most the guest's currently armed transfer size.** If a
transaction exceeds it the excess is truncated; the device logs this via
`LOG_GUEST_ERROR` (visible with `-d guest_errors`), but the host will see a
short accept and, if it resends the remainder, corrupt the stream.

Concretely: usbmuxd's default `USB_MTU` of 49152 is larger than both limits.
Left unchanged it split every large AFC write, and the guest's mux driver
reassembled unrelated fragments — uploads arrived at the correct size with the
wrong contents, and `installation_proxy` then failed to extract them. The fix is
host-side: keep the MTU at or below `max_transaction`.

## Required event order

```
hello  ->  reset  ->  (guest programs DCFG and EP0)  ->  enumdone  ->  SETUP on EP0
```

The guest parks with `GINTMSK = RESET | ENUMDONE` once its driver has brought
the core up, which is the state the host is expected to drive from.

## Data path

The device model is **DMA-only**, which is correct for iOS: `AppleSynopsysOTG2`
programs `GAHBCFG` with `DMAEN` set and never touches `GRXSTSP`. Payloads are
read from and written to guest memory at the address the guest programmed in
`DIEPDMA`/`DOEPDMA`.

The controller's FIFO registers (`GRXFSIZ`, `DIEPTXF`) are **word counts, not
byte limits**, and are not part of the data path in DMA mode. Do not clamp
transfer sizes against them.

## Threading

Callbacks run from main-loop fd handlers with the BQL held, so
`cpu_physical_memory_read`/`write` are legal there. Do not add threads.

## Implementation notes

`TCP_NODELAY` is required on both ends: every control transfer is a round trip,
and Nagle adds 40 ms to each.

The device is transfer-oriented and has **no concept of a control transfer** —
SETUP, data and status are three independent endpoint transactions, and the
guest's own USB stack supplies the descriptors. A host bridge therefore performs
standard enumeration by hand rather than issuing control requests.

Because the device never speaks unprompted, no file descriptor ever becomes
readable to announce incoming data; IN transfers only happen when the host
polls.

## Testing without an emulator

`fake_device.py` in the host repository replays this protocol, which lets the
backend be exercised without a five-minute boot.
`contrib/ipod-touch-usbhost.py` in this repository is the reverse: a minimal
host bridge that drives `reset -> enumdone -> GET_DESCRIPTOR(DEVICE)`.
