# iPhone 4 (N90, GSM) baseband: the HSIC link carries tracing, not cellular

The question was what the 4.x kernel expects on USB HSIC for the baseband. Short answer: on the
GSM iPhone 4 (iPhone3,1, X-Gold 618), HSIC only carries Infineon's **trace** interfaces. The
cellular path (AT, mux, SMS, calls, PDP) runs over **SPI2 with the IFX SPI protocol, version 2**.
That is the same kernel stack as the 3GS (`commcenter-4.2.1-3gs.md`), with a newer framing.
Emulating calls and SMS therefore needs no USB device. (HSIC as the main link is the CDMA iPhone 4,
N92 with a Qualcomm MDM6600, which is out of scope.)

Sources: N90 DeviceTrees 8A293/8C148/8L1, and the kernelcaches 8C148 and 8L1. Both kernels carry
the same personalities.

## HSIC side

| piece | what |
|-------|------|
| `usb-complex/usb-ehci` | `hsic-ports` 0x6 (ports 1 and 2), `hsic-devices-info` 0x00010001 0x2 0x00020001 0x3 |
| AppleUSBHSIC | AppleUSBHubHSICManager: StartDevice/StopDevice/enableDevice/disableDevice per hsic port |
| AppleBasebandUSB | personality **IFX-Tracing-Device**: IOUSBDevice by device class, class AppleBasebandUSBDevice (just SuspendDevice) |
| AppleBasebandCDC | personalities **IFX-Tracing-Interface3** and **-Interface5**: IOUSBInterface idVendor **0x1519** idProduct **0x0020**, bInterfaceNumber 3 and 5; bulk in/out pipes exposed as a devfs node by AppleBasebandCDCBSDClient |
| `baseband` node | `function-bb_usb_mux` (PMU GPIO 3), pmu_exton, bb_ldo; CommCenter's "Setting BB USB VBUS", `fExternalUSBTracing`, EnableBBUSB |

CommCenter enables BB USB only for baseband trace capture (`+trace=...`, `+xtdev`,
`+XTRACECONFIG`). Without the device the trace path stays off and nothing waits for it. If the USB
side is ever wanted, the minimum is a 0x1519:0x0020 composite device with CDC-style bulk interfaces
3 and 5 that accept and discard trace data. That is a YAGNI item until someone needs baseband trace
logs.

## The real link: SPI2, IFX protocol v2

| `arm-io/spi2` (8C148) | value |
|-----------------------|-------|
| compatible | `spi,s5l8930x,baseband|spi,s5l8920x,baseband`, reg 0x02200000, irq 0x0c |
| protocol-version | **2** |
| max-data-size | **0x7fc** |
| rx/tx-buffer-count | 16 / 16 |
| DMA | CDMA ch 0x10 (TX FIFO 0x82200010), 0x11 (RX FIFO 0x82200020) |
| MRDY / SRDY | GPIO 0x0605 (out) / GPIO 0x0104 (in, edge) |
| baseband node | bb_rst 0x0102, radio_on 0x0101, reset_det 0x0103, bb_on = PMU GPIO 2 |

v2 header (BasebandSPIIFXProtocolVersion2, rx path at kext 0x80677a88 in the 8C148a 3GS cache; the
same code is in the N90 caches):

| bytes | meaning |
|-------|---------|
| 0, 1[3:0] | payload length (12 bits; > max-data-size = invalid frame, 0xfff = none) |
| 1 bit 4 | more |
| 1 bit 5 | rx error (the kext reports it) |
| 2, 3[3:0] | **credits granted** to the receiver of this header, added up (`creditsGranted`) |

The AP sends data frames only while it holds credits, so the modem must grant some early. The core
(`ios_bb_ifx_*`, version 2) grants 8 when the AP's balance (modelled per data frame received)
drops below 4, and requests SRDY while a grant is due. **(black-box)** once N90 boots: whether
credits are consumed per frame or per buffer, and the initial-grant timing the kext expects.

Above the framing, everything matches the 3GS: AppleSerialMultiplexer runs 27.010 in the kernel and
CommCenter uses `/dev/mux.spi-baseband`, with the AT dialect in `commcenter-4.2.1-3gs.md`. Wiring
on N90 is the 3GS's spi2 baseband model at the A4's addresses, with `ifx-version=2`.
