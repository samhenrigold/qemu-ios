# N72 combo-card factory identity

N72's generated NOR contains a unit Wi-Fi and Bluetooth address. The BCM4325
card previously exposed a fixed Wi-Fi MAC in CISTPL_FUNCE and duplicated that
address in SDPCM event frames. Provision the card with `-M iPod-Touch,wifi-mac=...`
and use that same card address for its NIC and events. Unconfigured old callers
retain the previous identity. Factory provisioning cannot change after start.

The stock 5F138 AppleBCM4325 driver reads the common CIS before firmware download.
Native readback with Wi-Fi on confirms the provided address in both its Ethernet
log and lockdown. This fixes Wi-Fi identity; it does not yet fix Bluetooth identity.

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
Supplying that actual card record is the next model fix; changing the synthetic
identity to match the driver's fallback or patching the driver would hide it.

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
