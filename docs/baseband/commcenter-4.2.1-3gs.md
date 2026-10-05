# iPhone 3GS (N88) on iOS 4.2.1 (8C148a): how the baseband is attached and what CommCenter speaks

These notes come from the N88 8C148a DeviceTree and kernelcache, plus CommCenter 4.2.1 (armv7, 900096
bytes). In CommCenter, `__TEXT` is at 0x1000 = file offset 0. Addresses are notes for the next reader;
no model keys on them. Status: the static picture is complete. The parts marked **(black-box)** still
need a booted N88 to confirm. Compare with `commcenter-1.0.md` (M68): most of the AT layer carries over
unchanged, but the transport underneath is entirely different.

## Transport stack

```
CommCenter -> /dev/mux.spi-baseband (+ per-DLCI nodes via ASMIOCCREATEDLCI)
           -> AppleSerialMultiplexer (27.010 in the KERNEL, MuxIOSPIAdapter)
           -> BasebandSPI (BasebandSPIIFXProtocolVersion1, MRDY/SRDY)
           -> SPI2 + CDMA DMA -> X-Gold 608
```

* There is **no UART and no H5** on the 3GS data path. CommCenter (0x100ac) calls `stat()` on
  `/dev/mux.spi-baseband` first and only falls back to `/dev/mux.h5-baseband` (the M68/UART layout).
  `uart2/umts` (0x82700000) is the baseband's debug/flash UART (`/dev/uart.umts`, used by bbupdater
  and core dumps), not the AT path. `uart1/debug` is the AP console.
* AT before the mux and `+cmux` are unchanged (CommCenter 0x37b96: if the config wants a mux, send
  `+cmux=0,0,0,%d` and queue `e0`/`+cmee=1`/`+cscs="HEX"` on every dispatcher). The kernel runs the
  27.010 basic-mode framing (flag 0xF9, kReceivingSABM / kSendingModemBits / kReceivingUIH states,
  "Dumping 27.010 buffer"). On the modem side the core's existing mux engine therefore applies
  byte-for-byte. **(black-box)**: whether the kernel waits for a modem-originated MSC
  ("kReceivingModemBits"); the core echoes MSC as a response today.
* The packet-data DLCIs become kernel ifnets (MuxNetworkInterface; ASMIOCPDPIFCREATE/ACTIVATE), so
  the cellular-data stretch goal is IP over a DLCI, not PPP in CommCenter.

### DeviceTree (8C148a)

| node | what |
|------|------|
| `arm-io/spi2` `spi,s5l8920x,baseband` | reg 0x02200000 (+0x80000000), irq 0x9c, protocol-version **1**, max-data-size **0x7f8**, rx/tx-buffer-count 16, CDMA ch 0x10 (TX, FIFO 0x82200010) / 0x11 (RX, FIFO 0x82200020), config `0 0x53 0x20010100`, MRDY = GPIO 0x1802 (out), SRDY = GPIO 0x1304 (in, edge irq), fail_gpio 0x0804 |
| `baseband` `baseband,n88` | irq 0xa8; bb_rst GPIO 0x1407, radio_on 0x1405, reset_det 0x1500 (in), bb_on = PMU GPIO 2, umts_rxd_ctrl 0x0f03, bb_usart0_rxd_ctrl 0x0f02, `device-imei` and `snum` empty (lockdownd reads device-imei, so the board should fill it) |
| `uart2/umts` | 0x82700000 irq 0x16, the baseband's debug UART |
| `i2s2/audio2` `audio-data,baseband` | voice PCM (not modelled: no audio) |

AppleBaseband (kext 0x8066d000) has the classes AppleBasebandN82, N88 and N90. They look up
bb_on/bb_rst/radio_on/reset_det and the usb-mux functions; CommCenter drives them through
AppleBasebandUserClient (radio on, quiesce, powering down).

### IFX SPI framing (BasebandSPI-133.1, v1)

This is Infineon's SPI protocol, the same one Linux's `ifx6x60` driver implements. Every transfer is
full duplex, and each direction starts with a 4-byte header:

| bytes | AP -> modem and modem -> AP (v1) |
|-------|-----------------------------------|
| 0, 1[3:0] | payload length of this frame (12 bits); 0xfff = no data |
| 1 bit 4 | more: the sender has another frame queued (rx: kext ORs "transfer again") |
| 2, 3[3:0] | next_data_size |
| 3 bit 6 | CTS (kext rx 0x80678e28 sets its flag 0x10000) |

From the receive path (kext 0x80678d9e): `len = b0 | (b1 & 0xf) << 8`. If `len > max-data-size`,
the frame is invalid; lengths of 0xfff and above are dropped silently. The payload starts at byte 4
and goes to the mux. Handshake lines: MRDY (AP out) and SRDY (modem out, edge interrupt). The modem
raises SRDY when it wants to be clocked; the AP raises MRDY to start a transfer ("Tx/Rx DMA
(SRDYHI/SRDYLO)" states, "SRDY Timeout"). **(black-box)**: the transfer length per frame (the
header plus max-data-size, or the payload size rounded up) and the SPI word size, which comes from
config 0x20010100.

The SPI2 block is the same S5L89xx SPI as spi0/spi1, but the baseband driver
(AppleS5L8920XBasebandSPIController: SPCLKCON/SPCON/SPSTA/SPCNT/SPTDCNT) runs it from CDMA
descriptors instead of PIO. `hw/arm/ipod_touch_spi.c` is PIO only, so spi2 needs DMA pacing.
**(black-box)**: the register sequence.

Static read of AppleS5L8920XBasebandSPIController. The MMIO base is at object+0xc8, and the
register layout matches `ipod_touch_spi.c`:
* loadConfiguration: word size from the DT config (8/16/32 -> CFG bits 15-16 = 0/1/2). The CFG
  shadow is `0x2000 | ws << 15 | 0xc0000` (bits 18-19 are DMA request enables, absent in the PIO
  model).
* Per transfer (0x80676c10): CTRL = 0xc (FIFO resets), PIN = 0, RXCNT = TXCNT = 0,
  CLKDIV = shadow 0xf4, WORD_DELAY = 0xf8, +0x3c = 0xffffffff, +0x40 = 0, STATUS = 0x1f,
  RXCNT = len in words, then with TX data TXCNT = words and CFG = shadow | 0x40, or for
  receive-only CFG = shadow | 0x41 (bit 0: clock out dummy data).
* Start (0x80676b84): CTRL = 1 (RUN). Stop (0x80676bdc): CFG = shadow, CTRL = 0.

Because the IFX reply does not depend on what the AP sends in the same frame, the controller model
can produce MISO when the RX DMA first reads RXDATA, and consume MOSI once TXCNT words have arrived.
That works whichever DMA channel the driver starts first, and the CDMA model can stay synchronous.

v2 (iPhone 4) changes bytes 2-3 into a credit grant (`creditsGranted += b2 | (b3&0xf)<<8`, kext
0x80677af0) and adds rx_err at byte 1 bit 5. See `iphone4-hsic.md`.

## AT dialect: 4.2.1 compared with 1.0

The parser infrastructure is the same as 1.0's: `getInt(field, line, base)` at 0x88274,
`has(field, line)` at 0x881ac, the raw response string at 0x881e8, and URC registration as
(tag, object, member function).

| feature | 4.2.1 | vs 1.0 |
|---------|-------|--------|
| init | `at`, `e0`, `+xgendata`, `+xdrv=10,2`, `+xdrv=10,6` (boardId), `+cgsn`, `+xdrv=9,3`, `+cmux=0,0,0,N`, `+xpow=5,N,0`, `+cfun=1/4` | adds `+xdrv=10,6`, `+xnonce?`, `+xencr`, `+xtrlev=1`, `+xlog`, `+xia=1`, `+xfdor=1` (the core answers OK) |
| registration | `+creg=2`, `+cgreg=2`, `+xreg=1`, `+cops=0`, `+cops=3,2`, `+xcops=7/10`; handler 0x3ce78 parses `+CREG` exactly as 1.0 (n,stat,lac,ci / stat,lac,ci / n,stat / stat; lac/ci hex) | `+XREG`, `+XCGREG` and `+XSYSERR` URCs are new |
| **signal** | once registered (`+CREG` stat 1/5 sets 0xb6), `+xmer=1` then a poll of **`+xcgedpage=0,1`** (0x3b940, re-armed). The response handler 0x3dd48 searches the text for `RSCP:` (UMTS, dBm = -n), otherwise `Rssi:` (GSM RXLEV, dBm = n - 110, 0..63), and `RAT:` up to the next comma: `"GSM"` = 0, `"UMTS"` = 2. UMTS then polls `ECN0:`. | **New.** 1.0 took rssi from `+XCIEV` field 0. The core answers `+XCGEDPAGE: RAT:"GSM",Rssi: <rxlev>` |
| battery | `+XCIEV` handler 0x4f7e8 reads field 1 only (1..100) | the same field; field 0 is now ignored, so `+XCIEV: rssi,batt` serves both versions |
| operator | `+cops=3,2`, then `+cops?` (handler 0x3e218), `+xcops=10`, `+xcops=7` (0x3e4e8). The helper is `getStr(out, resp, enc, field, line)` at 0x882fc. `+COPS?` takes field 2 twice, as enc 7 (name) and enc 3 (hex -> atoi = PLMN), and field 3 as AcT (base 10; 2 = UTRAN starts the ECN0 path). `+XCOPS` takes field 1 as enc 3 (hex) = name | the same fields as 1.0. Leave AcT out (GSM) |
| calls | `+xcallstat=1`, `+clip=1`, `+ccwa=1`, `+cnap=1`, `+cusd=1`, `+cssn=1,1`, `+xemc=1`; `+XCALLSTAT: id,stat` (handler 0x2de50, getInt 0/1 base 10, the same Infineon states); `+CLIP` handler 0x2f58c (field 0 number, 1 type, 5 validity) | unchanged; adds `+XEMC`, `+COLP`, `+CSSI/U` |
| SMS | `+cnmi=1,2,2,1`, `+csms=1`, `+cmms`; `+CMT` (0x4cd74: PDU from line 1, then `+cnma`), `+CMTI` (0x4e094: index from field 1, mem "SM" -> `+cmgr`, then `+cnma`, `+cmgd`) | the `+CMT: ,<len>\r\n<pdu>` form is unchanged |
| SIM | `+cpin?`, `+xsimstate=1`, `+ccid`, `+cimi`, `+xpincnt`, `+clck="FD",2`, `+XSIM`/`+XLOCK` URCs, `+crsm`/`+csim` reads | adds `+XLOCK` and `+crsm` file reads (the core answers OK with no data) |

The `+XCIEV` field-0 difference is the only place 1.0 and 4.2.1 would want different replies, and
since 4.2.1 ignores field 0, one dialect serves both. That is why no per-firmware profile switch
exists yet. Add one when a black-box trace shows two firmwares parsing the same reply differently.

## Confirmed black-box on N90 4.2.1 (the same CommCenter and kernel stack, IFX v2)

These hold for the 3GS unless its v1 trace says otherwise.

* **Controller**: the RX chain (CDMA ch 0x11 on +0x20) is armed before any transfer. Each frame
  writes CTRL=0xc, then the counts, TXCNT=RXCNT=0x200 words (0x800 bytes = header + 0x7fc), TX
  DMA go, CFG = 0xd405a (bit 6 = go; CTRL RUN is written only after SRDY), MRDY up ... MRDY down,
  CFG = 0xd401a, CTRL=0. A modem-initiated frame is CFG 0xd405b: receive-only, no TX, no MRDY.
* **Handshake**: every frame needs a fresh SRDY rising edge after it is set up. An SRDY timeout
  with SRDY already high is fatal (kASMFatalErrorSPI 0x8; the mux goes Bypass -> Error). The
  kernel follows a frame whose "more" bit is set with a receive-only frame, so the modem must raise
  SRDY for any frame that is set up, not only when it has data. SRDY raised at boot before the
  kernel's first transfer also ends in that timeout.
* **v2 header bits**: AP byte 1 bit 6 means "I hold no credits" (it sends `00 40 01 00` and gets a
  grant on the next frame). The AP grants the modem 15 credits (`00 00 0f 00`); the modem sends data
  only against them.
* **Mux**: after `+cmux=0,0,0,1500` the kernel runs basic mode straight on the IFX payload (no H5),
  with 0xF9 flags on every frame, and it opens DLCIs 0..13. UA from the modem carries C/R=1.
* **Channel roles** (4.2.1): 1 = calls + SIM (`+xcallstat`, `+cpin?`, `+cimi`, `+xsimstate`),
  2 = registration/signal (`+creg`, `+xreg`, `+xmer`, `+xcgedpage` every ~5 s), 3 = SMS (`+cnmi`,
  `+CMT`; `+cnma` comes back on 4), 4 = `+xia`, 5 = PDP control, 7 = a second "radio" dispatcher
  (`+xgendata`, `+xdrv=7,...`), and 8+ = PDP data (the first context used DLCI 8).
* **Radio**: 4.x never sends `+cfun=1` at start (the modem boots on). `+cfun=6`/`+cfun=7` come from
  the SIM toolkit and are not radio off.
* **Calls**: answer is `ata`, end is `+chld=1` (ATH also handled), dial is `atd<num>;`.
* **Data**: `+cgdcont=1,"ip",""`, `+xgauth=1,1,"",""`, `+xdns=1,1`, `+cgact=1,1`, `+xdns?`,
  `+cgpaddr=1`, then `+cgdata="M-RAW_IP",1` on DLCI 8 (CONNECT), and raw IPv4 after that. Without
  `+XREG` > 2 (the data bearer; 4 shows "3G"), Safari says "Could not activate cellular data
  network" and nothing is sent.
* **27.010 modem status**: the kernel MSCs every DLCI and waits for the modem's own MSC
  (RTC|RTR). On the data DLCI, DV (carrier) must go up after CONNECT, and NO CARRIER plus DV down
  must follow `+cgact=0`. Without them CommCenter tore the boot-time PDP context down 100 ms after
  CONNECT and reset the baseband about 10 s later (2 boots in 3).
* **Frames**: the kernel's idle state is a pre-armed receive-only frame, so the modem's MISO is
  built when the clock runs (SRDY up), not at go.
* **The kernel's frame styles**: (a) MRDY up after go: AP-initiated; (b) TX frame with RUN and no
  MRDY, queued back to back while data flows: it waits for the modem's SRDY; (c) a receive-only
  frame with RUN (CFG bit 0): the idle state, ended by the modem's SRDY. While (c) is armed the
  kernel starts nothing itself, so a lost v2 credit deadlocks both sides. The modem tops the AP up to
  16 credits on every frame and clocks an idle (c) every 2 s.
* **Baseband reset**: CommCenter pulses bb_rst (GPIO 0x0102) and then radio_on (0x0101) low. The
  modem resets to raw AT and CommCenter's recovery re-runs init in bypass ("at" pings, then
  `+cmux`). A baseband reset by CommCenter (raw `at`
  pings after the mux was up) is not modelled. It only happened while the frame bugs above were
  still in.

How to watch it: `IOS_BB_TRACE=2` prints every AT line and reply, the SPI frames and MRDY/SRDY with
virtual-ms stamps. `S5L8930_CDMA_TRACE=1` prints the channel go's.

## Black-box checklist (N88, once it boots)

1. MMIO trace of spi2 and CDMA channels 0x10/0x11: word size, frame length, the MRDY/SRDY order.
2. `IOS_BB_TRACE=1`: the AT commands CommCenter actually sends after `+cmux`, and which DLCIs the
   kernel opens (SABM order) and how they map to call/reg/sms.
3. `/var/wireless/Library/Logs/CommCenter.log` (or syslog): "Reported signal strength is %d" and
   "telling UI to draw %d bars" confirm the xcgedpage path.
