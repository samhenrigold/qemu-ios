# CommCenter 1.0 (1A543a) and the baseband it expects

Static analysis of `/System/Library/Frameworks/CoreTelephony.framework/Support/CommCenter`
(iPhone OS 1.0, armv6, 378248 bytes) and of the AppleOnboardSerial kext in the 1.0 kernelcache.
CommCenter's own code is Thumb, symbols are stripped except the coalesced template instances
(`AtDispatcher::send<...>`, `GsmRadioModule::persist*`). Addresses below are CommCenter VM addresses
(text at 0x1000 = file offset 0) and are notes for the next reader, not something any model keys on.

This is what `hw/misc/ios_baseband_core.c` implements. Everything here is "what the 1.0 host sends and
what it parses"; where Infineon's real behavior is unknown the model picks the reply the parser accepts
and says so.

## Transport stack

```
CommCenter  ->  /dev/h5.baseband  (AppleReliableSerialLayer, kernel)  ->  UART1  ->  baseband
            AT lines | 27.010 basic mux | H5 (3-wire UART) | raw bytes
```

* Device: `GsmRadioModule` is constructed with radio type **1** (constant at 0x11bca). The
  per-type config (`setType`, 0x28460) for type 1: mux on, N1 = 1500, local baud 115200, target baud
  750000, H5 allowed (`cfg+0x24`), `fSupportsPowerSave`. Types 3/4 (a Telit "GM862 PCS" dev modem) run
  without mux; nothing selects them on a phone.
* `DarwinIpcDriver::open` (0x14974): `open(path, O_RDWR|O_NONBLOCK)`, `TIOCEXCL`, then
  `ioctl(IOAOSH5, &0)`. Success of that ioctl is `fSupportsH5TransportMode`; on h5.baseband it always
  succeeds. cfmakeraw, 115200, CRTSCTS.
* **H5 cannot be refused.** After the baud step CommCenter (0x12fd0) does
  `if (cfg.h5 && driver.supportsH5) { driver.setH5(1); send "+xtransportmode" }`. The kernel
  (AppleReliableSerialLayer write path, kext 0xc039a5bc) snoops a write starting with
  `at+xtransportmode`, enables H5 and starts link establishment. The `+xtransportmode` reply handler
  (0x12f8c) continues init only on OK; ERROR leaves init with no next step. So the modem must speak H5.
* The kernel's H5 is the Bluetooth 3-wire UART link:
  * SLIP: 0xC0 delimits, 0xDB 0xDC = C0, 0xDB 0xDD = DB.
  * Header: `b0 = seq | ack<<3 | crc<<6 | reliable<<7`, `b1 = type | (len & 0xf) << 4`, `b2 = len >> 4`,
    `b3 = ~(b0+b1+b2)`.
  * Link control (type 15, unreliable): SYNC `01 7e`, SYNC RESP `02 7d`, CONFIG `03 fc <cfg>`,
    CONFIG RESP `04 7b <cfg>`. The kernel sends cfg 0x17: window 7, no OOF, data-integrity CRC on.
    It reaches Active after SYNC RESP then CONFIG RESP; data before Active is dropped.
  * Data: type **14**, reliable (packets without the reliable bit are rejected). Pure acks are type 0.
  * CRC (when hdr bit 6 set): `crc = 0xffff; crc = T[b ^ (crc >> 8)] ^ (crc << 8)` with T the reflected
    CCITT (0x8408) table **byte-swapped per entry**, final value bit-reversed, sent big-endian, over
    header + payload. (Not the textbook H5 CRC; reproduced from kext 0xc039b550 and its table.)
  * Kernel rx buffer is 0x5e3 bytes; window 7.
* 27.010 basic mode (CSIMultiplexer, 0x1a18a...): flag 0xF9, FCS = 27.010 CRC-8. Control field
  compared with P/F masked: SABM 2F, UA 63, DM 0F, DISC 43, UIH EF, UI 03 (UI accepted, UIH sent).
  DLCI 0 carries MSC/PSC/CLD/Test/NSC. Power save: `Enter low power mode` sends PSC; exit sends wake-up
  flags and gives up after 50 tries ("baseband not responding to wakeup flags"), so a sleeping modem
  must answer flags with flags.

## Channels

Built in the GsmRadioModule ctor (0x13e60): AtDispatcher "default" on the raw driver, then on the mux

| DLCI | name    | used by |
|------|---------|---------|
| 1    | call    | call list model, radio module (+cfun, +xdrv), DTMF |
| 2    | reg     | registration model (+creg/+cops/+xciev...), battery monitor (+xciev field 1) |
| 3    | sms     | SMS store/message models, SIM model (cpin/cimi/ccid), cell broadcast |
| 4    | low     | settings, phonebook, STK, audio routing |
| 5    | pdp_ctl | PDP context control (+cgdcont/+cgact/+cgdata) |

PDP data DLCIs are allocated from 6 up at runtime. URCs arrive on the channel where they were enabled
(e.g. `+creg=2` is sent on DLCI 2), which is how the model routes them.

## AT framing

* Commands: `"at" + printf(fmt) + "\r"` (0x30258); always lowercase format strings.
* Responses: lines; final `OK`, `ERROR`, `+CME ERROR: n`, `+CMS ERROR: n`, `CONNECT`, `BUSY`,
  `NO CARRIER`; `RING` is special-cased. Tags compare **case-insensitively** (0x30088), so the modem
  answers in the usual upper case.
* Fields (locator 0x2f0b6, verified by running it under unicorn): `TAG: f0,f1,...`, quoted strings
  returned without quotes, field -1 = the tag, line index = n-th response line. Getters:
  `getInt(field, line, base)` 0x2f420, `has(field, line)` 0x2f4a6, `getStr(enc, field, line)` 0x2f524.
  getStr with enc >= 1 hex-decodes first (the host set `+cscs="HEX"`), enc 0 is raw.

## Init sequence (default dispatcher, raw then H5)

1. `at` ping, 2 s timeout (sendBulk); 5 misses -> driver reset (AppleBaseband reset GPIO), 5 resets ->
   "Radio isn't responding, giving up".
2. `e0`, `+xsio?` -> expects `+XSIO: <cur>,*<new>`; field 1 after its first char must equal the wanted
   value (0). Different -> `+xsio=0`, `+cpwroff`, power cycle.
3. ping again, then `e0`, `+ipr=750000` (sendBulk 500 ms) -> local baud change.
4. `+xtransportmode` (H5 switch above) -> `e0`, `+cmee=1`, `+cscs="HEX"`.
5. `+xgendata` -> on OK `+xdrv=10,2`; version = from the first digit after the first `"` to the next `"`
   (`"DEV_ICE_MODEM_03.12.08_G"` -> `03.12.08_G`). `+xl1set="psvon"/"psvoff"`, `+cmee=1`,
   `+cscs="HEX"`, `+cgsn` (field 0 raw = IMEI), `+xdrv=9,3` (optional: `+XDRV: 9,3,0,"<17 chars>"`).
6. `+cmux=0,0,0,1500` -> on OK "Multiplexer started", SABM DLCI 0..5. Already queued on every DLCI:
   `e0`, `+cmee=1`, `+cscs="HEX"`. Then `+xpow=5,250,0` on DLCI 1, then `+cfun=4` (airplane) or the
   radio-on path (`+cfun=1`, peripheral power `+xdrv=7,0,n,m`).

## SIM (GsmSimModel)

* `+XSIM: <n>` URC (Infineon states): 0 = removed, 7 = ignored, 8 = special, others -> first time
  marks inserted and sends `+cpin?`.
* `+cpin?` -> `+CPIN: READY` (enc 0). READY on an Infineon config -> `+cimi` (simple) and
  `+xsimstate=1` -> `+ccid`. `+CME ERROR: 14` retries, 10 = not inserted, 13/15/23 = failure; also
  SIM PIN / SIM PUK / PH-* strings.
* `+cimi`: field 0 raw IMSI ("Home PLMN is %d MCC %d MNC %d"). `+ccid`: field 0 raw ICCID.
* `+xpincnt`: field 0 = PIN1 attempts, field 2 = PUK1 attempts (`+XPINCNT: 3,3,10,10`).
* `+clck="FD",2`: field 0 compared with "0" (FDN off) -> `+cimi`.

## Registration (GsmRegistrationModel, DLCI 2)

* Enables: `+xmer=1`, `+creg=2`, `+cgreg=1`, `+ctzr=1`, `+ctzu=1`, `+cops=0`, `+cops=3,2`.
* `+CREG`: both `n,stat[,"lac","ci"]` (query) and `stat[,"lac","ci"]` (URC), lac/ci hex.
* `+cops?` -> field 2 with enc 3 (hex) = numeric PLMN: `+COPS: 0,2,"<hex of 310410>"`.
* `+xcops=7` / `+xcops=<n>`: field 0 type, field 1 name (enc 3, hex).
* `+xeons`: network list (fields 1-4, hex); only used for manual selection.
* Signal: **`+XCIEV: <rssi>,<batt>`**, no +CSQ. Field 0 rssi 0..31 -> dBm `2n-113` (0 -> -113,
  1 -> -110, 31 -> -51), larger values ignored. Field 1 = battery capacity 1..100 for IfxBatteryMonitor.
* Also handled: `+CGREG`, `+XCREG`, `+CTZV`, `+CTZDST`, `+XNITZINFO`.

## Calls (GsmCallListModel, DLCI 1)

* Enables: `+xcallstat=1`, `+clip=1`, `+ccwa=1`, `+cnap=1`, `+cusd=1`, `+cssn=1,1`, `+csvm?`.
* `+XCALLSTAT: <id>,<stat>` (Infineon: 0 active, 1 held, 2 dialing, 3 alerting, 4 incoming, 5 waiting,
  6 released, 7 connected); 6 is special-cased.
* `+CLIP`: field 0 number (enc 0, raw), field 1 type, field 5 CLI validity.
* Dial `d<num>;`, answer via `+chld=2`, hang up `+chld=1<id>` / `+chld=0..4`, `+ceer`, `+clcc` (sent
  without a parser), DTMF `+vts`/`+xvts`.
