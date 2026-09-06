# Machine configuration

Behavior options are moving to typed QEMU machine properties. This migration is
incremental; most existing `IT_*` variables still retain their documented behavior.

| Property | Values | Default | Legacy alias |
| --- | --- | --- | --- |
| `audio-hw` | `auto`, `on`, `off` | `auto`: CS42L58 and AMC present for direct iBoot, absent otherwise | `IT_AUDIO_HW`: leading `0` disables; any other value enables |

Use `-M iPod-Touch,audio-hw=on` to force audio hardware. An explicitly supplied
property, including `auto`, wins over the environment alias. The alias remains
compatible and emits a deprecation warning when used. The option controls
hardware presence, not output volume or decoding, and is immutable after the
machine starts. `-M iPod-Touch,help` lists the property and its type.

The regression harness accepts `--audio-hw auto|on|off`. Omitting it preserves
the existing environment/default path. `tests/ipod/test_audio_config.py` checks
default behavior, alias precedence, explicit auto, invalid values and rejection
of changes after startup.

## Firmware profiles

Build-specific addresses live in `ipod_touch_firmware.c`, shared by MBX, FMSS
and the machine's task-port patch. Loaded-kernel detection matches the complete,
NUL-terminated 5F138 or 7E18 Darwin banner, not merely Darwin 9/10. An empty early
scan retries; positive results are cached until CPU reset. Ambiguous images are
rejected.

| Build | Boot-argument buffer | Clock function (physical) | AMFI task / task-name (virtual) |
| --- | --- | --- | --- |
| 5F138 | `0x0ff2a584` | `0x0816b460` | Not mapped |
| 7E18 | Early iBoot handoff in the machine | `0x081953e0` (native PMU RTC, no MBX patch) | `0xc01ab200` / `0xc01ab2a0`, slide `0xb8000000` |

The MBX-triggered clock/BCM4325/USB kernel modifications are 5F138-only. Unknown
kernels and 7E18 receive none of those hardcoded writes. The AMFI task-port
patch uses detected 7E18 addresses, retaining its instruction check. Research
builds without a mapped profile require all three explicit `IT_AMFI_HOOK_SLIDE`,
`IT_AMFI_GET_TASK_NAME_VA`, `IT_AMFI_GET_TASK_VA` overrides; partial overrides
still work for a recognized mapped kernel.

This is the shared-address and patch-safety portion of the firmware work. A
user-selectable firmware property and the remaining peripheral defaults are
still pending. AES output-address exceptions remain unclassified and were not
moved into a profile without evidence. FMSS's Bluetooth-node fix already searches
its anchor; it does not need another hardcoded address.

`test_firmware_profiles.py` runs the production detector and patch guards under
ASan/UBSan. `test_agent_guest.py --firmware --base-nand .../nand-agent-v4` checks
7E18 boot and verifies that a legacy MBX register read leaves five kernel regions
unchanged. The 5F138 profile is checked against its local decrypted kernel banner
and guarded patch test; a fresh native 5F138 boot remains unverified here.
