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
