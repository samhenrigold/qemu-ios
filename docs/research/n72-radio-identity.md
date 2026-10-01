# N72 combo-card factory identity

N72's generated NOR contains a unit Wi-Fi and Bluetooth address. The BCM4325
card previously exposed a fixed Wi-Fi MAC in CISTPL_FUNCE and duplicated that
address in SDPCM event frames. Provision the card with `-M iPod-Touch,wifi-mac=...`
and use that same card address for its NIC and events. Unconfigured old callers
retain the previous identity. Factory provisioning cannot change after start.

The stock 5F138 AppleBCM4325 driver reads the common CIS before firmware download.
Native readback with Wi-Fi on confirms the provided address in both its Ethernet
log and lockdown. `bt-mac` also provisions the combo card's Apple OTP record.
Both addresses remain immutable factory data across reset.

## Bluetooth lead

The 5F138 kernel's `populateDeviceTree` copies the WLAN address to the Bluetooth
property, incrementing its first byte, when the driver's Bluetooth address is
zero. This is Apple's fallback, rather than a NOR provisioning error. A read-only
debugger trace proves stock iBoot successfully copies the intended `btaddr` into
its DeviceTree. With Wi-Fi off, lockdown reports that address. With Wi-Fi on, the
running IORegistry instead contains the driver's fallback.

The driver's OTP parser accepts a vendor CIS tuple (code 0x80, subtype 0x81).
Its Apple configuration records contain a little-endian 16-bit type and length
in 16-bit words. Type 3, five words, supplies six Bluetooth address bytes.
The model now supplies that actual card record when `bt-mac` is configured.
Native 5F138 lockdown and IORegistry readbacks match all four generated identity
values with Wi-Fi on. No kernel instruction patch or changed identity scheme is
needed. This retains the existing iBoot UART-node workaround and HCI stand-in.

All eight default native regression checks pass on a current 7E18 prepared base
and guest offer, including GL, audio, installation, launch, clean persistence and
fsck. The old `nand-current` fixture still selects its legacy GL engine and fails
two unimplemented fixture calls, even if a new OpenGLES file is staged; that
unrelated old-path failure is retained, not counted as a passing regression.

Evidence outside the product:

- `/private/tmp/ltm-n72-card-mac-qtest.log`: production CMD52 CIS reads, reset,
  legacy default and immutable runtime provisioning, 2/2 pass.
- `/private/tmp/ltm-n72-211-card-mac-native/identity-readback.json`: provisioned
  Wi-Fi readback and remaining Bluetooth mismatch.
- `/private/tmp/ltm-n72-211-bt-env-gdb/result.json`: stock iBoot copies the
  generated Bluetooth address from NOR.
- `/private/tmp/ltm-n72-211-card-mac-ioreg2/ioreg.txt`: guest DeviceTree after
  Wi-Fi starts, read through a scratch probe built with the existing legacy ABI.
- `/private/tmp/ltm-211-bcm-cis-disassembly.txt`: stock driver OTP parsing and
  provisioning. Research instruction addresses are not emulator behavior.
- `/private/tmp/ltm-n72-combo-otp-qtest.log`: 3/3 production SDIO card checks.
- `/private/tmp/ltm-n72-211-combo-otp-native/`: all four generated lockdown
  identity readbacks and matching Bluetooth DeviceTree property.
- `/private/tmp/ltm-n72-combo-otp-prepared-regress/`: all eight default checks.
