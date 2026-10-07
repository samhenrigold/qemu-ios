# Stock iBoot NVRAM to kernel handoff

2026-09-30 bounded native controls isolate the firmware handoff from emulator
command-line injections. All runs use private NOR copies and NAND overlays;
shared prepared devices remain unchanged. These are research traces, not a
claim that an unmodified stock device reaches an activated home screen.

## Method and concrete results

Replace only the normal CHRP `common` partition in the copied NOR with the
existing identity variables and this standard NVRAM configuration:

```
boot-args=serial=1 debug=0x8 rd=disk0s1 -v ltm_nvram_probe=314159
```

The existing `imgtools/build_nor.py:nvram_bank` produces the checksums, partition
lengths and generation. Stop the guest through QMP at 10, 30 and 60 seconds,
read physical RAM `[0x08000000,0x09000000)`, then resume. Match the actual
revision-one kernel `boot_args` structure, including physical and virtual
bases, and record `CommandLine` at offset 0x38. Finding a marker elsewhere in
RAM alone does not establish kernel consumption.

| Board/build | Boot control | Actual kernel boot_args | CommandLine |
| --- | --- | --- | --- |
| N45 / 3A101a | ordinary board boot, no host argument injector | PA 0x08661000, version 3 | full marker plus trailing space |
| N45 / 4B1 | ordinary board boot, no host argument injector | PA 0x0866c000, version 4 | full marker plus trailing space |
| N72 / 5F138 | stock decrypted direct-iBoot, `boot-args=` | PA 0x08820000, version 2 | one space |
| N72 / 7E18 | stock decrypted direct-iBoot, `boot-args=` | PA 0x087ad000, version 5 | one space |

The N45 results persist across all three observations and reach the kernel's
`BSD root: disk0s1` mount. Both old and newer N45 firmware consume the same
format; there is no justification for a new N45 argument injection.

Both N72 versions retain the empty handoff at all three observations. The
5F138 control also prints the empty `gBootArgs.commandLine`, with no observed
heap panic. Its marker appears elsewhere in RAM; only the 7E18 debugger run
independently proves import into the live environment.

The N72 controls intentionally use direct-iBoot: an empty `boot-args` machine
property disables the late timer and direct-iBoot literal injection, but the
SecureROM path still has an unconditional compatibility data write. Running
SecureROM with that property alone would not be a stock argument control.
Direct-iBoot also bypasses ROM/LLB and synthesizes the image epoch; it does not
certify the real-chain security policy.

## N72 selection boundary, measured with the debugger

A second 7E18 run installs a QEMU hardware debugger breakpoint at stock iBoot
VA 0x0ff11b40, before formatting `gBootArgs.commandLine`. No guest instructions
or data are changed. The live environment list contains:

```
node 0x0ff31868: boot-args = serial=1 debug=0x8 rd=disk0s1 -v ltm_nvram_probe=314159
secure-boot = 0x1
boot-command = fsboot
```

At that same stop, r6 = 0x0ff1dba0 points to an empty string, and the ramdisk
selection global at 0x0ff25040 is zero. Stock code at 0x0ff11a72 tests that
global and selects the empty source at 0x0ff11a7c; the alternative source is
ramdisk-specific. The formatting path then prints `gBootArgs.commandLine =
[ ]`. Thus the NOR bytes were transferred, parsed and imported successfully;
this observed failure to propagate arguments occurs after environment import.
The earlier claim that any NOR arguments necessarily heap-panic 7E18 is too
broad: this control reaches the handoff without that panic.

This does not establish that a hardware register is wrong. Do not modify NOR
layout, bypass a security bit, or overwrite the selected pointer to obtain a
passing result. The next implementation decision requires tracing who writes
the normal source buffer (0x0ff1dba0 for this image), comparing that path with
real stock boot policy, and identifying any actual board read that diverges.
If production iBoot deliberately excludes these arguments, automatic emulator
additions must stop relying on an invented NVRAM-to-AMFI propagation contract;
that is a provisioning compatibility boundary, not a NAND/SPI repair.

## Retained evidence and reproducibility

Each `result.json` records complete argv, base NOR SHA256, actual NVRAM keys,
physical boot_args addresses and every observation; adjacent `ram-*.bin`,
serial logs and screen captures retain the raw observations:

- `/private/tmp/ltm-stock-args-n45-3A101a/`
- `/private/tmp/ltm-stock-args-n45-4B1/`
- `/private/tmp/ltm-stock-args-n72-5F138/`
- `/private/tmp/ltm-stock-args-n72-7E18/`
- `/private/tmp/ltm-stock-args-n72-gdb/result.json` retains the breakpoint,
  registers, parsed live environment list and selection global.
- `/private/tmp/ltm-stock-args-probe.py` and `ltm-stock-args-gdb.py` are the exact
  disposable research drivers; their hardcoded paths are not product APIs.

## Loaded-host unlock seam

`MT_TRACE=1` native K48 boot/unlock and 70001-byte clean shutdown/reboot
persistence both passed together in 70.9 seconds at
`/private/tmp/ltm-stock-args-loaded-k48/`. This is a two-guest concurrent
regression, not a reproduction or causal explanation of the earlier failed
loaded-host run at `/private/tmp/ltm-nand-ownership-final-native/`.
That retained failure has no touch trace and ends with a black framebuffer
following the unlock gesture. The acceptance seam remains open: reproduce it
with touch reporting and LCD sleep/wake traces, then compare event ordering
before changing IRQs, gestures or timing. A subsequent isolated or concurrent
pass must not erase the failed evidence.
