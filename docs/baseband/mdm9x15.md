# Qualcomm MDM9x15 transport

The iPhone 5 connects `usb-mdm9x15` and a reliable H5 UART transport to the
shared `IosBasebandState`. The shared core owns SIM identity, registration,
signal, SMS, calls and GPS. Transport-specific clients, transaction identifiers,
routes and subscriptions remain owned by their UART or USB connection.

## Firmware and startup

FirmwareKit selects the original Mav5 archive, metadata and expanded MBNs from
the matching BuildManifest. The native updater reads them from
`/usr/local/standalone/firmware/Baseband/Mav5/`.

The download device enumerates as `05c6:9001`; runtime uses `05c6:9034`.
DLOAD/Sahara validate complete payloads before entering runtime. Carrier
configuration and PRI state come from the client's committed upload. A modem
reset clears incomplete downloads and negotiation state.

The radio begins network search when the firmware reaches runtime. Search
completion follows the shared core's SIM/radio readiness and search deadline.
Allocating a NAS client observes that state.

## UART and USB ownership

UART1 carries the native H5 connection. HSIC/QMUX uses physical USB port 2.
Transport resets release that transport's clients and subscriptions while
preserving the independent connection.

H5 router acknowledgement is negotiated. The client may configure routes
without waiting for responses; version-query negotiation enables the router's
acknowledgement mode. Route state is committed in either case. Serial reset
clears both routes and the negotiated response mode.

The UART DMA byte counter wraps over the streaming descriptor ring. Receive
budget and queued bytes remain distinct: a zero seed is a valid starting
counter, and backpressure must preserve it. Receive notifications follow
actual available bytes and descriptor progress.

## Runtime services

| Service | Model contract |
| --- | --- |
| CTL | Allocate/release clients, service capability and indication policy |
| DMS and UIM | Device/SIM identity and readiness |
| NAS | Shared radio registration, network and signal state |
| VOICE | Dial, answer, hold, connected status and hangup |
| WMS | SMS routes, submission, inbox, transfer and acknowledgement |
| PDS | Location sessions, fix indications and satellite-report subscriptions |
| WDS | Service setup and packet-data state |

Service replies preserve transaction and client identity. USB endpoint and
UART queues are bounded, and pending indications retry after backpressure or
suspend filtering permits delivery.

Outgoing calls connect inside the simulated network and remain active until
hangup. Incoming calls publish All Call Status to allocated VOICE clients;
Answer Call updates the same shared call state.

WMS SetRoutes selects discard, stored notification, transfer-only or transfer
with modem acknowledgement by message class. Transferred format-6 GSM TPDUs
exclude the SMSC prefix. Stored Raw Read returns the SMSC-prefixed form.
Transfer-only delivery requires the matching native Send ACK transaction.
Inbox reuse invalidates old ownership tokens.

CTL suspend-indication configuration is tracked per client. Packet admission
and packet consumption are separate operations. A filtered or full queue
retains the indication's delivery state for the next permitted attempt.

## Host controls

The shared modem object is `/machine/baseband-modem`. QMP `qom-set` uses the
following properties:

| Property | Value |
| --- | --- |
| `incoming-call` | Caller number |
| `incoming-sms` | `number|text` |
| `remote-hangup` | The shared modem's remote call-release control |
| `gps-fix` | `latitude,longitude[,altitude[,speed[,course[,accuracy]]]]` |

`call-state` exposes the current call state. `/machine/baseband-usb` exposes
transport startup and attachment state; attachment alone does not establish
NAS registration. See [GPS](gps.md) for location units and PDS wire layouts.

## Diagnostics and tests

`usb_mdm9x15_qmi` records packet input and output admission. Outcomes are -1
for input, 0 for rejected output, 1 for queued output and 2 for output consumed
by the suspend filter. Admission does not establish guest consumption.

`usb_mdm9x15_registration` records registration result, virtual time, search
deadline, radio mode and SIM readiness. Flag bits 0–5 describe powered off,
manually detached, network available, SIM present, PIN enabled and PIN entered.
Both events are disabled by default.

`tests/slice/a6-mdm-*.c` covers USB control, download, QMUX routing, reset,
reconnection, serial negotiation, SMS and PDS. Shared phone-state and NMEA
checks are in `tests/unit/test-ios-baseband.c`. Runtime checks use native
Phone, Messages and CoreLocation clients on the [N42 builds](../n42/README.md).
