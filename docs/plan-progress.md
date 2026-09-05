# Emulator and Light Touch plan progress

The workstream follows the supplied hardware-fidelity plan, with reproducible
storage corruption promoted ahead of new peripherals. Each completed item needs
both a focused regression and relevant guest evidence. Bluetooth peers and
TV-out were explicitly deferred by that plan.

| Track | Status | Acceptance evidence still needed |
| --- | --- | --- |
| Generated NAND integrity | In progress; `a27930d426` relocates the legacy free-block pool beyond the GPT volume and reads packed GPT capacity correctly | Repeated Coldplay/Spore installs, system-file integrity, respring, clean shutdown and cold boot; diagnose the later guest reset |
| PMU ADC and masked event IRQ | Pending | 7E18 conversion result and completion IRQ; event masking/read-to-clear; boot/shutdown |
| Battery controls | Pending | Calibrated guest percentage and charging state for runtime host changes |
| Headset/Mikey detection | Pending | Plug/unplug and headset button traces; correct guest routing |
| Microphone/I2S RX | Pending | Deterministic input tone captured by the guest, then host microphone recording |
| Native idle sleep/wake | Pending | Auto-lock, actual display/CPU sleep, power/Home wake without brightness overrides |
| Kernel serial console | Early boot arguments fixed | XNU output on serial and in exported diagnostics |
| Settings Wi-Fi join | Pending | Manual join without known-network seed or alert loop; DHCP and traffic |
| Two-instance LAN | Pending | Separate identities, MACs and state; bidirectional traffic between guests |
| Hardware shortcuts | Pending | Trace-backed PKE, NOR persistence, FMSS completion/erase, DSI response queue, LCD status and AES matching review |
| Light Touch integration | Original UI fixes committed | Package and verify the resulting hardware controls and readiness/recovery paths |
| Bluetooth peers / TV-out | Deferred per supplied plan | No implementation claimed |

See [UI reliability](ipod-ui-reliability.md) for the original fixes and the
storage reproduction. Temporary `/tmp` paths there identify local evidence;
they are not dependencies of the committed tests.
