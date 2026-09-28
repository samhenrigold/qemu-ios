# Native shutdown on 7E18

## Verified fix (2026-09-04)

SpringBoard calls `reboot2(RB_HALT, NULL)`, which asks launchd to coordinate
shutdown. The former `sync(); reboot(RB_HALT)` helper bypassed that path.
`ithalt` now uses the same native entry point as SpringBoard. The resulting
guest writes `0x90` to PMU register `0x6f`; the PMU now honors that command
without requiring the host to arm a UI power gesture first. No guest-PC
heuristic or timeout is used to declare shutdown.

The isolated `boot,persist,fsck` regression passed using the rebuilt helper,
QEMU, and usbmuxd bridge: two guest-origin SHUTDOWN events, byte-identical
persistence after reboot, and a full 1,835,008-block HFS check. The volume
attributes were `0x80000100`, including the clean-unmount bit.

```sh
python3 tests/ipod/test_pmu_shutdown.py
python3 tests/ipod/test_armv6_toolchain.py
python3 tests/ipod/regress.py --checks boot,persist,fsck \
  --qemu build-native14/qemu-build/qemu-system-arm \
  --usbmuxd build-native14/build/usbmuxd/src/usbmuxd \
  --base-nand ../qemu-ios-files/nand-ultimate
```

The private usbmuxd listener also needs preflight to connect to its own
address: `-S` alone previously left libimobiledevice connecting to the system
daemon. The bridge now sets its internal `USBMUXD_SOCKET_ADDRESS` before
preflight starts. This regression passed without an inherited override.

## Earlier raw-syscall investigation

The following records the failure before the fix, not the current helper.

Investigated 2026-09-04 using the `nand-ultimate` image, 3.1.3/7E18,
`contrib/it-halt/ithalt`, and an isolated temporary write overlay.

`ithalt` invokes `sync(); reboot(RB_HALT)` and prints its message **before**
entering the reboot syscall. That message, SSH disconnection, a two-second
wait, or QEMU exit status zero is not evidence that the PMU powered off.
In particular, externally sending QEMU SIGTERM also produces exit status zero.
Use the QMP SHUTDOWN event with `guest=true` and `reason=guest-shutdown`, or the
embedded `qemu_ios_ui_guest_shutdown_confirmed()` latch, to prove that event.

## Runtime evidence

The ordinary regression reached a home screen, staged a persistence marker,
then hung after the SSH halt request. PMU tracing ended with the following
register accesses; there was no write of `0x90` to register `0x6f`:

```
write 30=00, 31=00, 1d=12
read 10=e0; write 10=e0
read 0a=10, 34=80
write 0a=18, 33=00, 34=80, 61=00
read 61=00, 63=00, 62=00, 60=00
```

Three CPU register snapshots two seconds apart showed `PC=c00697aa`,
`LR=c031d849`, `R0=ffffffff`, `R1=00000000`, `CPSR=00000033`.
The kernel is in a terminal branch loop, not waiting for another PMU response.

## Kernel evidence

The local IPSW kernelcache was decrypted using the already-present 7E18
kernel IV/key in `hw/arm/ipod_touch_aes.c`. Decompression yielded 7,757,824
bytes; the `complzss` header length and Adler-32 checksum both matched.
The extracted Mach-O is `/tmp/kernel-7e18.macho`, SHA-256
`8caf1738b15fe4df99ddebb1f976b8582364af27ef738b93dcac60be1b26c63e`.
The extraction script used for this investigation is `/tmp/decode-7e18.py`.
Its Mach-O symbol table identifies `c0069796` as `_halt_all_cpus`:

```
c006979a  cmp  r0, #0
c006979c  beq  c00697ac
c006979e  movs r0, #0
c00697a0  bl   __consume_printf_args
c00697a4  movs r0, #1
c00697a6  bl   _PEHaltRestart
c00697aa  b    c00697aa
c00697ac  movs r0, #0
c00697ae  bl   __consume_printf_args
c00697b2  movs r0, #0
c00697b4  bl   _PEHaltRestart
c00697b8  b    c00697aa
```

The PMU's `shutdown_armed` gate is therefore not the cause of this native
halt hanging: the standby command never arrives. Recognizing this particular guest PC as a synthetic power-off would not
solve that failure. The later launchd path above supplies the actual command.

At this point the unresolved issue was how the raw halt path should signal a
completed shutdown to the host. The syscall reached its terminal halt loop;
the observed register snapshots alone do not prove an unmount or establish
an image-independent hardware completion signal. Keep persistence regression
and app shutdown results honest until that signal is modeled or the guest
uses a verified power-off path.

Raw investigation artifacts were written to `/tmp/ipod-pmu-halt-20260904`
and `/tmp/ipod-pmu-halt-regs2`; the ordinary full-volume post-run fsck passed,
but that does not turn the missing shutdown event into a successful test.
