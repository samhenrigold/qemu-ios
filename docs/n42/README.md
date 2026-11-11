# iPhone 5 (N42AP, S5L8950/A6)

Core contributed by Steven Troughton-Smith.

The `iPhone-5` machine boots a FirmwareKit-prepared N42 kernel bundle and NAND
through launchd to SpringBoard. The board has two `apple-swift` application
processors, 1 GiB of DRAM and a 640×1136 display. UIKit uses a 320×568 point
coordinate space. Device models and the host OpenGL ES bridge are shared with
the other machines where their register and transport contracts agree.

## Supported builds

The following builds have completed fresh preparation and reached the Home
screen on their first normal boot. Each has also run stock applications,
joined the virtual Wi-Fi network and loaded web content in Safari.

| iOS | Build | Board |
| --- | --- | --- |
| 6.0 | 10A405 | N42AP |
| 7.0 | 11A465 | N42AP |
| 8.0 | 12A365 | N42AP |
| 9.0 | 13A344 | N42AP |
| 10.3.4 | 14G61 | N42AP |

The shared implementation provides touch, hardware buttons, sleep/wake, a
full charging battery, speaker audio, simulated calls and SMS, and GPS fixes
through native CoreLocation. Camera capture uses generated sensor images.
Validation applies to the listed builds; additional builds require their own
preparation and runtime checks.

## Build and launch

Use the repository's [build instructions](../../README.md#build). The host
codec dependencies used by the shared video models also apply to this target.
H.264 camera encoding requires FFmpeg with a libx264 encoder.

For an already prepared device:

```sh
build/qemu-system-arm \
  -M "iPhone-5,kboot=$DEVICE/kboot.bin,nand=$DEVICE/nand,ppn=on,baseband=on,camera=on,quiet-boot=on" \
  -m 1G -accel tcg,thread=multi \
  -display cocoa \
  -audio driver=coreaudio,out.frequency=48000 \
  -device usb-bcm-hsic,bus=usb-bus.0,port=4,mac="$WIFI_MAC" \
  -netdev user,id=wifi0 \
  -serial null \
  -qmp unix:/tmp/n42-qmp.sock,server=on,wait=off
```

`DEVICE` identifies the prepared device directory. `WIFI_MAC` must match the
WLAN address in its device tree, including the nested USB WLAN node and
`/chosen/mac-address-wifi0`. Keep the machine's CPU topology: QEMU reserves
four execution contexts for the application processors and auxiliary engines.
Use the normal TCG clock for interactive operation.

`quiet-boot=on` removes verbose flags from the in-memory boot handoff. The
prepared KBoot file remains usable for diagnostic launches. Display, PMU,
USB host, audio ports, backlight and battery models are enabled by default.
The launch above also enables the modem and camera.

## Board and device models

| Component | Implementation | Board contract |
| --- | --- | --- |
| Application processors | `apple-swift`, ARM target | Two application cores; DRAM at 0x80000000 |
| Interrupts and timers | AIC and ARM generic timers | AIC at 0x3f200000, 192 IRQs; counter and deadline conversions use the same clock period |
| GPIO | Shared S5L8930 GPIO | 29 ports, eight interrupt groups, released-input and pad-level behavior |
| Touch | Shared SPI and multitouch model | SPI1 at 0x32100000, IRQ29; Zephyr profile, ATN GPIO201 |
| I2C | PA Semi command-FIFO variant | Controllers at 0x33200000–0x33400000, IRQ18–20 |
| PMU | D1972 model | I2C0 address 0x74, GPIO6 interrupt; ADC, thermal inputs, cable state, wake and shutdown |
| Battery | Shared HDQ/bq27540 | UART5 at 0x32a00000; virtual 1440 mAh pack with external charging power |
| Accessory and backlight | TriStar and backlight models | I2C0 addresses 0x1a and 0x63 |
| USB device | Shared PHY and DWC OTG | PHY at 0x36000000; OTG at 0x36100000, IRQ11 |
| USB host | Shared EHCI/OHCI | EHCI at 0x36400000, IRQ12; modem on physical port 2, WLAN on port 4 |
| WLAN | `usb-bcm-hsic`, shared Broadcom FullMAC | BCM4334 download and runtime transport, CDC/BDC commands, Ethernet and QEMU network backend |
| Modem | `usb-mdm9x15`, shared cellular model | Qualcomm download and QMUX protocols; UART1 at 0x32600000, IRQ22 |
| Console | Shared Samsung UART | UART6 at 0x32b00000, IRQ27 |
| DMA | Shared CDMA variant | 44 channels; UART receive budgeting and audio pacing |
| Storage | Shared H2FMI and NAND store | PPN operation, persistent NAND, storage-IOP control and firmware execution |
| Display | Shared H5P display variant | Display engines at 0x3a100000 and 0x39200000; DSI at 0x39500000 |
| Scalers and DART | Shared scaler and DART models | Scalers at 0x38300000/0x38400000; DART at 0x38b00000 |
| Graphics | Shared OpenGLES bridge | Guest GL dispatch, host acceleration and IOSurface synchronization |
| Audio | MCA, I2S/CDMA, CS42L65 and CS35L19 | Speaker PCM, codec control, amplifier and DMA transport |
| Camera | H4 ISP, shared JPEG and Topaz models | Generated front/rear sensors, still capture, H.264 encoding and native playback |
| Guest services | Shared agent, package and pasteboard transports | Typed RPC, source-built guest helpers and versioned guest packages |

The peripheral models implement register and protocol behavior consumed by
the native drivers. Timing-sensitive transfers use the corresponding model's
clock and queue state. Guest-controlled descriptors, lengths, addresses and
allocation sizes must pass the shared bounds checks before use.

## Firmware preparation

FirmwareKit owns firmware selection, executable preparation, guest-package
installation and initial filesystem provisioning. A prepared device combines
matching kernel, device tree, boot arguments, System contents, Data seed,
keybag and trust metadata.

The companion FirmwareKit change provides `firmwarekit-prepare` for the N42
preparation stages. Its package README documents the required inputs, output
files and native initialization completion checks. The calling host currently
orchestrates these stages; the catalog-driven `firmwarekit create` dispatcher
does not yet select N42. Fresh preparation installs the guest-service helpers.
The separate `firmwarekit-provision-guest-tools` driver upgrades stopped legacy
images and does not implement the N42 NAND output route.

The preparation contract is:

1. Preserve N42 product identity and the original kernel/device-tree pairing.
   Populate the boot handoff and generate a fresh device-tree random seed.
2. Restore allocated filesystem contents exactly, including zero-filled
   blocks. Provision the PPN layout and persistent device metadata together.
3. Initialize protected Data with the native filesystem and MobileKeyBag
   services. Preserve ownership, modes and executable entitlements.
4. Generate the capability cache with the matching System libraries and apply
   the supported activation and finished-setup preparation for that build.
5. Install source-built guest packages using the shared preparation pipeline.
   iOS 8 and later require the matching launchd metadata and agent job policy.
6. Complete native filesystem shutdown before starting the normal guest.

The 16 GB layout uses a 3 GiB System partition beginning at 16 MiB and gives
Data the remaining 12,761,997,312 bytes. Filesystem capacity must match the
partition extent. FirmwareKit's storage-layout and native restore components
provide these dimensions and verify the restored byte stream.

For iOS 10, first-boot preparation supplies source-generated container metadata
for the MobileGestalt and configuration-profile SystemGroups. The initial
root-owned capability cache is readable by mobile. Build-specific migrator
edits retain the prepared records while allowing the remaining migrations to
run. FirmwareKit validates the original input hashes and the prepared signed
pages, and preserves executable sizes and entitlements.

User applications install through MobileInstallation onto Data. The agent
provides installation and launch control through the shared guest-service
transport. See [AppSync](../../contrib/appsync/README.md) and
[restore helpers](../../contrib/it-restore/README.md) for their source builds.

## Input and power

The digitizer profile maps absolute display coordinates into the native sensor
range with inverted sensor Y. Reports carry contact identity, lifecycle and
edge-origin information. Contact release and cancellation must retire the
active path even when the host pointer leaves the display. Multi-contact
reports share the same transport and coordinate calibration.

Hardware buttons drive the GPIO pads and the PMU wake path. A sleeping
application processor resumes through the PMU transition. The iOS 10 lock
screen uses Home to wake and a second press to open.

`system_powerdown` requests native guest shutdown. The emulator exits when the
PMU reports standby or completes the charging-halt transition. Persistence
checks require that completion before reopening the same NAND.

Battery reporting combines the HDQ gauge, PMU cable state, TriStar attachment
and the USB host's charging-current grant. The virtual pack reports healthy,
full and charging. Separate PMU thermal inputs represent a room-temperature
device and support normal camera flash operation.

## Networking, cellular and location

The WLAN transport shares `bcm_fullmac.c` with the earlier boards. The HSIC
wrapper supplies BCM4334 download framing, runtime identity, ready/reset GPIOs
and MAC provisioning. The native driver associates with `qemu-ios`, obtains a
DHCP lease and exchanges Ethernet frames through the configured QEMU backend.

The modem uses the shared phone state behind Qualcomm UART/USB transports.
Its boot protocol validates the original Mav5 payloads supplied during
preparation. Runtime service negotiation, router acknowledgement, carrier
configuration and reset state follow the native client's requests.

The simulated cellular network accepts outgoing SMS, connects calls until
hangup, and supports injected incoming calls and SMS. These events remain
inside the virtual network. Controls on `/machine/baseband-modem` include:

| Property | Value |
| --- | --- |
| `incoming-call` | Caller number |
| `incoming-sms` | `number|text` |
| `gps-fix` | `latitude,longitude[,altitude[,speed[,course[,accuracy]]]]` |
| `call-state` | Read-only call state |

Coordinates use degrees, altitude uses metres above WGS84, speed uses m/s,
course uses degrees clockwise from north and accuracy uses metres. An empty
`gps-fix` clears the fix. Native PDS sessions deliver location through
CoreLocation. See [MDM9x15](../baseband/mdm9x15.md) and
[GPS](../baseband/gps.md) for protocol details.

## Audio, camera and graphics

Speaker output uses the MCA/CDMA PCM path and the shared host audio backend.
The guest frame clock and host output sample rate are converted by the audio
model. Native incoming-call handling also uses the modeled audio ports.

The camera model generates deterministic front/rear patterns and metadata.
Rear stills are 3264×2448; front stills are 1280×960. The ISP implements channel
configuration, autofocus/exposure acknowledgement, image/statistics completion
and rear preflash/main-flash sequencing. Preview uses the video overlay and
scalers; stills use the shared JPEG engines; Topaz uses host H.264 compression.
Native Camera Roll displays the resulting photos and movies.

The graphics bridge implements ES state, buffer mapping, vertex arrays,
program uniforms, texture storage, queries, clip distances and framebuffer
fetch. IOSurface imports and readbacks preserve format, row stride, mapping
permissions and completed rendering. Cached dispatch adapters select the
correct per-context entry points in later guests.

Coherent framebuffer fetch preserves the order of overlapping primitives.
The CGL fast path uses texture barriers; large triangle draws can capture
post-transform positions and group conservatively independent primitives.
Host-only draw sections release QEMU's shared lock while retaining exclusive
renderer ownership. Guest-memory accesses and final synchronization remain
under the lock. The same implementation serves every supported board.

## Current limitations

- 3D satellite imagery supplied as HEIC material textures renders with fallback
  textures in the listed legacy guests.
- Photos editing has a reported black/warped rendering case on iOS 10.
- Native host microphone capture and full audio routing require further
  end-to-end qualification.
- The storage IOP executes with a Cortex-A9 ISA model; its physical core
  identity and complete timing fidelity require further hardware evidence.

## Validation

Run the repository gates and focused host tests:

```sh
tests/gate.sh --quick
tests/gate.sh --models
```

Relevant production slices are `tests/slice/a6-*.c`,
`tests/slice/bcm-fullmac-*.c`, `tests/slice/gles-*.c`, the shared display/DMA
checks and `tests/unit/test-ios-baseband.c`. Accelerated graphics checks need
host GPU access. The native draw-batching tests compare complete-frame pixels,
occlusion results and restored bindings against the per-primitive path.
The execution-gate test checks I/O progress, serialized GL callers and fault
retry cleanup with real host threads.

For each prepared build, verify:

1. First normal boot reaches Home, and stock applications launch.
2. Touch, edge gestures, buttons, lock, sleep and wake work.
3. Battery reports full/charging; Wi-Fi connects and Safari loads a web page.
4. A user application installs, gets its container and launches.
5. Outgoing/incoming calls and SMS complete through native applications.
6. CoreLocation receives successive fixes and handles fix removal.
7. Front/rear photos, flash modes, movie capture and playback work.
8. Native shutdown persists changes through a subsequent cold boot.
