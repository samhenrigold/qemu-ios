# Machine configuration

Behavior options are moving to typed QEMU machine properties. This migration is
incremental; most existing `IT_*` variables still retain their documented behavior.

| Property | Values | Default | Legacy alias |
| --- | --- | --- | --- |
| `boot-args` | kernel command line, at most 255 bytes; empty disables injection | no override | none: the property is the only input |
| `boot-args-delay-ms` | 0..3600000 virtual milliseconds | `0` | none |
| `boot-args-repeat` | 0..1000000 writes; 0 still performs the initial write | `200` | none |
| `boot-args-interval-ms` | 1..3600000 virtual milliseconds | `250` | none |
| `bt` | `on`, `off` | `on` | `IT_BT`: leading `0` disables, otherwise enables |
| `bt-latency-us` | unsigned 32-bit microseconds | `2000` | `IT_BT_LATENCY_US` |
| `osk` | `on`, `off` | `off` | `IT_OSK`: any present value enables |
| `h264-decode`, `scaler-decode`, `mpvd-decode`, `lcd-planes` | `on`, `off` | `on`; `off` selects the legacy register stubs | none |
| `amc-mode` | `registers`, `handshake`, `decode` | `decode` | `IT_AMC_STATE` selects `handshake` |
| `audio-hw` | `auto`, `on`, `off` | `auto`: CS42L58, amp, I2S0 and AMC present on every boot (2.1.1 panics without them) | `IT_AUDIO_HW`: leading `0` disables; any other value enables |

Use `-M iPod-Touch,audio-hw=on` to force audio hardware. An explicitly supplied
property, including `auto`, wins over the environment alias. The alias remains
compatible and emits a deprecation warning when used. The option controls
hardware presence, not output volume or decoding, and is immutable after the
machine starts. `-M iPod-Touch,help` lists the property and its type.

The regression harness accepts `--audio-hw auto|on|off`. Omitting it preserves
the existing environment/default path. `tests/ipod/test_audio_config.py` checks
default behavior, alias precedence, explicit auto, invalid values and rejection
of changes after startup.

The legacy on-screen-keyboard tapper uses `osk=on`. Explicit `osk=off` overrides
any `IT_OSK` value, and the option cannot change after startup. The launcher’s
`--keyboard` / `--appsync` flags and Light Touch now use this property. The app
also passes its USB session through the existing `usb-tcp-addr` option rather
than changing process-wide `IT_USB_TCP`. Agent text insertion is unchanged.
`test_osk_config.py` checks alias presence, explicit precedence and immutability.

The existing Bluetooth HCI controller uses `bt` and `bt-latency-us`. Both are
startup-only, and explicit options override aliases. A user-supplied UART1
chardev still takes precedence over the built-in controller. Reply delay is
stored per controller and converted to nanoseconds without signed overflow;
invalid, negative and oversized legacy values are rejected. These controls do
not add Bluetooth peers. `test_bt_config.py` checks the real paused machine,
including aliases, boundaries and runtime rejection.

## Boot-argument scheduling

`boot-args` and its three scheduling properties are fixed before machine startup and
have no environment aliases (the `IT_BOOT_ARGS*` variables are ignored). The command
line is limited to the kernel buffer's 255 bytes; longer strings are rejected. An
empty command line disables injection. The string reaches the kernel two ways, both
derived from the staged iBoot image by pattern (hw/arm/it_iboot.c, any iPod touch 2G
build): its normal-boot command-line literal is redirected to the string before
iBoot hands off, and the kernel's `boot_args.CommandLine` is rewritten on the timer
until AMFI has latched it.

The first timer write occurs after `boot-args-delay-ms`. Later writes use
`boot-args-interval-ms`; an interval of zero is rejected to prevent a busy timer
loop. `boot-args-repeat` counts successful writes, including the first. As before,
zero still permits the initial write, and a pending AMFI task-port patch can keep
the shared timer active beyond that count. The independent AMFI option is
unchanged. Delays and intervals use guest virtual time, not wall-clock time.

Legacy scheduling aliases are resolved once during startup with strict bounded
integer parsing, and emit deprecation warnings. Explicit properties take priority
even over malformed aliases. Reset reuses these resolved settings. The
machine's defaults are 0 ms / 200 writes / 250 ms, what every iPod boot uses. The paused native matrix in
`test_time_dilation_config.py` covers defaults, aliases, explicit precedence,
boundaries, malformed input and runtime mutation rejection.

## Firmware profiles

There are none. The per-build kernel-banner table (`ipod_touch_firmware.c`, 5F138 and 7E18) had no caller
left and was deleted with its unit test; the build-specific addresses it once keyed (MBX, FMSS, the AMFI
task-port patch, the 5F138 clock trampoline) were removed before it. iBoot's boot-args literal, security
epoch and 2.x command line are found by pattern in any iBoot (`hw/arm/it_iboot.c`).
