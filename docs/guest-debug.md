# Debugging the guest: QEMU's gdbstub, lldb, and the guest kernel's own structures

The guest kernel, and any process running on it, can be debugged on every board with the host's stock
`lldb` (Xcode's; no gdb-multiarch needed) attached to QEMU's gdbstub. Nothing runs in the guest, so this
works on iPhone OS 1.0, which never had a debugserver. `imgtools/lldb/xnu.py` teaches lldb what a
debugger built for xnu would know: the process list, which process the CPU is running, a process's
dyld images, and breakpoints that stop in one process only.

Measured 2026-10-04 with lldb-2103 on 1.0 (M68, 1A543a), 3.1.3 (iPod touch 2G, 7E18) and 3.2.2 (iPad,
7B500), and 2026-10-05 on the newer boards (table below).

## Per board (2026-10-05)

Kernel: `xnu-procs` lists every process and `xnu-current` names the running one. User: `xnu-images --pid
<SpringBoard>` from a sysroot made with `dsc_extract.py`, then `xnu-break SpringBoard mach_msg` stops in SpringBoard
with symbolized library frames. `tests/ipad1/debug-check.py` runs both passes on the kboot boards.

| Board, build | Kernel | User | debugserver (DDI, `ddi-check.py`, 2026-10-06) |
|---|---|---|---|
| M68 1.0 1A543a | PASS (full symbols; r9 current thread) | PASS (52 images, plain rootfs as sysroot; libSystem/CoreFoundation frames) | none (1.0 never had one) |
| N18 5.1.1 9B206 | PASS (4047 exported names; offsets found) | PASS (215 images; local symbols kept: `__CFRunLoopServiceMachPort`) | PASS with the 5.1 DDI: attach 9 s, breakpoint, bt to the Harness main, detach, app alive. Its debugserver has no qRegisterInfo: lldb gets `arm_gdb_regs.py` |
| N81 6.1.6 10B500 | PASS | PASS (231 images; the cache's local symbols are `<redacted>`, exported ones named) | PASS with the 6.1 DDI: attach 13 s (36 s without `--sysroot`), all steps |
| N88 6.1.6 10B500 | PASS | PASS (231 images, `debug-check.py` all PASS) | PASS with the 6.1 DDI: attach 11 s, all steps |
| N90 4.2.1 8C148 | not run | not run | PASS with the 4.2 DDI: attach 13 s, all steps; this debugserver also has no qRegisterInfo, and reports its breakpoint trap with subcode 0, which lldb shows as EXC_BREAKPOINT |
| N90 7.1.2 11D257 | PASS | PASS (318 images, `debug-check.py` all PASS; `<redacted>` locals) | PASS with the 7.1 DDI (11D167), mounted by `ideviceimagemounter`: attach 25 s, all steps |

KERNEL for these: the decrypted kernelcache (FirmwareKit's cache, or `img3_decrypt` + `complzss` from
`imgtools/ipad1_fw.py` with the catalog row's `kernelcache` key). kboot does not slide the kernel, so lldb loads it
at its file address on 6.x and 7.x too. The sysroot: the decrypted rootfs's `dyld_shared_cache_armv7` through
`dsc_extract.py`, plus `usr/lib/dyld` and SpringBoard.

## debugserver from a DeveloperDiskImage (4.x to 7.x)

`tests/ipad1/ddi-check.py` runs the whole thing on a kboot board and is the recipe:

```
tests/ipad1/ddi-check.py --machine n88 --device DEV --product-version 6.1.6 \
    --ddi "xcode-ddi/ddi/6.1" --overlay OVL --sysroot ROOT --out OUT
```

- **Debuggable build.** The app must be signed with `get-task-allow` (the entitlement Xcode gives a development
  build). Without it debugserver's `task_for_pid` fails, lldb reports "attach failed: Error 1" and the app keeps
  running. `ddi-check.py --expect-refused` checks exactly that (PASS on N81 6.1.6 and N90 7.1.2, 2026-10-06). ddi-check
  re-signs the IPA with `ldid -S` and a one-key entitlements plist (or none, for the negative case).
- **Mount.** Xcode's DeveloperDiskImage.dmg and its .signature for the major.minor. On iOS < 7,
  libimobiledevice 1.4.0's `ideviceimagemounter` uploads the image over AFC, then prints "Unknown error occurred,
  can't mount" without ever sending MountImage (its AFC branch keeps a stale error). ddi-check then calls
  `mobile_image_mounter_mount_image` itself (ctypes) on `PublicStaging/staging.dimage`. On 7.x the tool works.
- **Setup and pairing.** SpringBoard launches no app while Setup Assistant is up, so ddi-check walks it
  (`app-install.py`'s `walk_setup`) the first time. OVL is a NAND overlay kept between runs, with a clean guest
  shutdown at the end. The host's pair records are kept in OVL.conf: a 7.x device that has paired once asks "Trust
  This Computer?" for a new host, and nothing answers it here. On 7.x, ddi-check waits for the guest's
  `it_boot: package` line before unlocking. it_boot starts the guest agent that the unlock asks, and file
  creation on the data volume (pairing, the AFC upload) can stall until then.
- **Launch.** The guest agent launches the app. Its pid comes from `launchctl list`
  (`UIKitApplication:<bundle>[..]`).
- **lldb.** `idevicedebugserverproxy` forwards com.apple.debugserver. `imgtools/lldb/dsattach.py` then connects
  and attaches by pid: these debugservers have no qfProcessInfo, so `attach -n` cannot work. It stops where the
  main thread's mach_msg returns (a status-bar tap wakes the run loop), prints the backtrace, continues and
  detaches. Things it handles:
  - **Registers.** The 4.x and 5.x debugservers answer qRegisterInfo with nothing, which leaves lldb with no
    registers. dsattach then loads `arm_gdb_regs.py`, the classic GDB ARM `g` layout (`lldb_shim.py`'s), as
    `plugin.process.gdb-remote.target-definition-file`. 6.x and 7.x answer an error until they have a process,
    then the layout.
  - **Breakpoint stops.** 4.2's debugserver reports the breakpoint trap with subcode 0, and lldb does not count
    that as a breakpoint. dsattach judges the stop by the pc instead.
  - **Async continue.** dsattach continues asynchronously, because a synchronous Continue blocks until the next
    stop.
- **Library symbols (ROOT).** Build ROOT from the build's rootfs: `dsc_extract.py` on
  `System/Library/Caches/com.apple.dyld/dyld_shared_cache_armv7`, plus `usr/lib/dyld`. dsattach maps `/System` and
  `/usr` there with `target modules search-paths` (`platform select remote-ios --sysroot` alone still read them
  from memory), and lldb matches them by UUID. Without ROOT every library is read from guest memory over USB:
  attach then takes 36 s instead of 11 s on N88 6.1.6 (about 100 s under load), and library frames have no names.
  The app binary is always read from guest memory (the Harness has no LC_UUID to match a host copy).
- **Killing lldb.** Killed while attached, lldb takes the app with it: debugserver kills its inferior. Always
  detach.

## Recipe

Start QEMU with a gdbstub on a local port. Add `-S` only to stop at the first instruction, before the
bootrom.

| Board | How |
|---|---|
| M68 1.0 | `RUN=/tmp/x tools/boot.py SECS --extra '-gdb tcp:127.0.0.1:PORT'` (qemu-ios-files/m68/tools) |
| iPod 2G 3.1.3 | the `iPod-Touch` command line from `tests/ipod/regress.py` plus `-gdb tcp:127.0.0.1:PORT`, with `IT_DIRECT_IBOOT`, `IT_TVOUT_READY=1` and `IT_LCD_BRIGHT=255` in the environment (`tests/ipod/regress.py` `boot_env`; without them it stays in iBoot) |
| iPad 3.2.2 | `IPAD1_QEMU_EXTRA='-gdb tcp:127.0.0.1:PORT' tests/ipad1/boot-smoke.py ...`, or the `ipad1` machine line it prints |
| N81, N90, N18, N88 (kboot) | `IPAD1_QEMU_EXTRA='-gdb tcp:127.0.0.1:PORT'` with any `tests/ipad1` harness and `--machine iPod-Touch-4G`, `iPhone-4`, `n18` or `n88 --device DEV`; `tests/ipad1/debug-check.py` does it and runs both passes. Target arch `armv7-apple-ios` |

Then:

```
lldb
(lldb) target create --arch armv6-apple-ios KERNEL       # armv7-apple-ios for the iPad
(lldb) gdb-remote 127.0.0.1:PORT
(lldb) command script import imgtools/lldb/xnu.py
(lldb) xnu-procs
  pid name              proc       task       pmap       ttb
   10 SpringBoard       0xc0bd56cc 0xc07777a8 0xc0778cc0 0x09184000
   ...
(lldb) breakpoint set -n unix_syscall
(lldb) continue
(lldb) xnu-current
kernel mode, pid 10 SpringBoard (proc 0xc0bd56cc, thread 0xc0ab9778)
(lldb) bt
   * frame #0: 0xc011f80c kernel.boot.macho`unix_syscall
     frame #1: 0xc005f160 kernel.boot.macho`fleh_swi + 192
     frame #2: 0x30404760 CoreFoundation`CFGetUserName + 8      (after xnu-images, below)
     ...
     frame #18: 0x323b75dc UIKit`UIApplicationMain + 304
(lldb) detach
```

KERNEL is the decrypted kernelcache: `qemu-ios-files/m68/m68_10/kernel.boot.macho` (1.0, full symbols),
`ipod-ipsw/scratch/7E18-dec/kernelcache.mach` (3.1.3, full symbols), `ipad1/7B500/dec/kernelcache.mach`
(3.2.2, about 4600 exported names). Always `detach`. A killed lldb leaves the guest paused, so reattach
and detach, or send QMP `cont`.

## xnu.py

| Command | What |
|---|---|
| `xnu-procs` | every process from `allproc`: pid, name, proc, task, pmap and the pmap's translation table (physical) |
| `xnu-current` | the process the CPU is in. Kernel mode: the current thread (r9 on 1.x, whose `current_thread` is `mov r0, r9`; TPIDRPRW on 3.x) -> task -> proc. User mode: the process whose pmap holds TTBR0 |
| `xnu-images --sysroot DIR [--pid N]` | a process's images from dyld's `dyld_all_image_infos` (the current process by default, any process with `--pid`, read through its own table). Each image is added to lldb at its load address from DIR, a host copy of the guest's root filesystem (on 3.x with the shared cache extracted by `dsc_extract.py`), which gives symbols to user frames |
| `xnu-break NAME ADDR\|SYMBOL` | a breakpoint that stops only when process NAME is running (kernel or user code) |
| `xnu-offsets [k=v ...]` | the offsets in use. Setting them by hand covers a kernel the discovery below cannot read |

No per-build tables: the offsets are read out of the kernel each session.

- **Accessors.** `get_threadtask` (or `current_task`), `get_bsdtask_info`, `get_task_map`, `get_task_pmap`
  and `proc_pid` are each one or two loads. The offsets come from their disassembly (Thumb `movs/lsls`
  immediates, `ldr`/`ldr.w`).
- **Name and task.** `p_comm` is where `kernproc` holds "kernel_task". `p->task` is the word in
  `kernproc` that equals `kernel_task`.
- **Translation table.** The pmap's physical translation table is the word in `*kernel_pmap` equal to
  TTBR1. `vm_map->pmap` is otherwise the word in `kernel_task->map` equal to `kernel_pmap`.
- **allproc on 7B500.** 7B500 does not export `allproc`. It is found from `kernproc`, the list's last
  entry, by following `p_list.le_prev` back to the first link that lies in the kernel's `__DATA`.

| Build | thread->task | task->proc | task->map | map->pmap | proc pid / comm / task |
|---|---|---|---|---|---|
| 1.0 1A543a (xnu-933) | 0x2fc | 0x178 | 0x14 | 0x24 | 0x24 / 0xeb / 0x138 |
| 3.1.3 7E18 (xnu-1357) | 0x34c | 0x1c4 | 0x14 | 0x24 | 0x8 / 0x14c / 0xc |
| 3.2.2 7B500 (xnu-1504) | 0x348 | 0x1c8 | 0x18 | 0x24 | 0x8 / 0x150 / 0xc |

(pmap: physical table at +4 on all three.)

## Userland

**Through the gdbstub, every firmware including 1.0.** Breakpoints and reads on user addresses use the
process that is current when they fire. The usual pattern:

```
(lldb) target modules add HOST/Hello.app/Hello            # the executable, if not in the sysroot
(lldb) target modules load --file Hello --slide 0         # 1.x-3.x executables are not slid
(lldb) xnu-break Hello "-[HelloApp sayHello]"
(lldb) continue                                           # tap in the guest
(lldb) xnu-images --sysroot ROOT                          # now UIKit & co. have symbols
(lldb) bt
   * frame #0: 0x000086e8 Hello`-[HelloApp sayHello]
     frame #1: 0x323c6e68 UIKit`-[UIView(Internal) _mouseUp:] + 52
     frame #2: 0x323bfbb4 UIKit`-[UIWindow _handleMouseUp:] + 156
     ...
     frame #16: 0x000088cc Hello`main + 148
```

Before a process exists (to catch its first instruction), use `xnu-break NAME ENTRY`. The name is
already the new one by the time dyld runs.

A crash in a process can be caught with `xnu-break NAME exception_triage`. On 1.0 the thread's saved
user registers are at `thread + 0x1b4` (r0-r12, sp, lr, pc, cpsr, fsr, far: `fleh_dataabt` stores them
there), with the thread in r9. The Hello app's first crash was found that way (docs/m68/sideload.md).

1.x and 2.x have no shared cache, so ROOT is simply the decrypted rootfs. From 4.3 dyld and the shared cache are
slid (ASLR): `xnu-images` then finds `dyld_all_image_infos` through the task (exec records its address there;
the word in the task that points at valid infos), and each image is added at the address dyld recorded. On 3.x the libraries live in
`dyld_shared_cache_armv6/7`, in the 2009 `dyld_v1` format that ipsw and modern tools no longer parse.
`imgtools/lldb/dsc_extract.py CACHE ROOT` writes each cached library back out as a Mach-O of its own:
the library's segments, a private `__LINKEDIT` holding its symbol table, cache addresses kept, and
`MH_DYLIB_IN_CACHE` cleared so lldb reads it as a plain file. Copy the rootfs's `usr/lib/dyld`, and any
app binaries you want symbols for, into ROOT as well. `xnu-images --sysroot ROOT` then works as it does
on 1.x. Measured:

```
3.2.2 (iPad, 7B500), 300 images; xnu-break SpringBoard mach_msg:
   * frame #0: 0x33aef708 libSystem.B.dylib`mach_msg
     frame #2: 0x309964fc MobileBluetooth`BTFrameworkIsServerUp + 24
     frame #4: 0x314d8d22 CoreFoundation`CFRunLoopRunSpecific + 2098
     frame #6: 0x3414e0da GraphicsServices`GSEventRunModal + 114
     frame #9: 0x3223f95a UIKit`UIApplicationMain + 642
3.1.3 (iPod 2G, 7E18), 273 images; xnu-break syslogd read stops at libSystem.B.dylib`read (user mode)
```

On 3.x kernels the kernel-to-user frame chain ends at the trap, unlike 1.0's. For user frames, stop in
user code (a library function) rather than in a kernel function.

**debugserver (3.x).** 3.1.3: the SDK's DeveloperDiskImage debugserver, through `imgtools/lldb/lldb_shim.py`
(imgtools/lldb/README.md, measured 2026-08-03 on the jailbroken development base). 3.2.2: the same
service after `mobile_image_mounter` mounts the 3.2.2 DDI (docs/ipad1/guest-services.md). It is not
exercised here.

## Gotchas

- **ARMv6 vs ARMv7.** The target arch picks the disassembler and register set: `armv6-apple-ios` for
  the S5L8900/8720 boards (ARM1176), `armv7-apple-ios` for the A4. The gdbstub's register layout (core
  plus VFP plus the cp15 system registers: TTBR0/1, TTBCR, CONTEXTIDR, TPIDRPRW...) comes from QEMU's
  target XML, so lldb needs no definition file.
- **Thumb.** lldb takes the mode from the symbol (N_ARM_THUMB_DEF). An address without a symbol is
  disassembled as ARM; use `disassemble -A thumbv7 -s ADDR` (or `thumbv6`) for Thumb code there. The
  1.x kernel and frameworks mix both.
- **Breakpoints are TCG's, not memory patches.** QEMU checks breakpoints against the virtual PC, so
  guest memory (and code-signing page hashes on 3.x) is never modified. The flip side: a breakpoint in a
  shared library fires in every process at that address. `xnu-break` filters by process, at the cost of
  a TTBR0 lookup on every hit (cached per table). On something every app calls per frame (the event
  handler), even that slows the guest to a crawl, so prefer the app's own code or a kernel function.
- **MMU.** gdbstub reads translate through the current TTBR with the current privilege, so a user-mode
  stop cannot read kernel memory through lldb (on 6.x the read even succeeds with zeros, so xnu.py walks every
  kernel address from a user-mode stop itself). xnu.py then walks the short-descriptor tables itself (L1
  sections and supersections, L2 small and large pages) and reads physical memory through QEMU's
  `qqemu.PhyMemMode`. That is also how it reads another process's memory (`--pid`). TTBCR.N is 2 on
  these kernels: user addresses below 1 GiB go through TTBR0, the rest through TTBR1.
- **Caches.** QEMU models none, so reads are coherent with what the CPU sees. There is no
  clean/invalidate to worry about after writing code.
- **Single-step under TCG.** `thread step-inst` and `thread step-over` work in kernel and user code (measured
  in `-[HelloApp sayHello]` on 1.0). QEMU's step mode masks IRQs and timers during a step
  (`process plugin packet send qqemu.sstep` answers 0x7), so a step does not wander into the timer
  interrupt.
- **Scripts (`lldb -b -s`).** `breakpoint delete` with no ID asks for confirmation. In a script the command
  after it then never ran and the session hung, so delete by ID. Always end with `detach`.
- **Time.** While stopped the VM is paused and its virtual clock stops, so the guest's watchdogs do not
  notice.
- **The kernel image.** lldb finds the kernel by itself only when the first stop is in kernel code. When
  the first stop is in user code or the vector page, xnu.py loads the kernel at slide 0 (no KASLR before
  iOS 6).
- **Early attach on 1.0.** One boot panicked ("ipc_mqueue_receive_results: strange wait_result")
  after repeated attaches before SpringBoard was up. Attaching after the home screen was fine in many
  sessions. In two M68 boots the guest stopped taking taps after an lldb was killed in the middle of a
  command (by its `timeout`). This was not root-caused. A boot with no debugger ran normally to 870 s, and a
  boot that went through attach, a user breakpoint, three steps and detach kept taking taps.
