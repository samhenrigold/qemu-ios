# kmem — userland kernel/physical reader for the 7B500 iPad (status)

Goal: read SoC registers / physical memory from the running jailbroken iPad 1
(7B500) over SSH, no DFU, to feed the emulator's PMGR/timer/GPIO/VIC/DRAM-mirror
model. Read-only on device.

## What works

**Getting native code to run on 3.2.2.** No on-device compiler, no cycript
runtime (only `cynject` + `libsubstrate` are present). Build an armv7 **dylib**
on the Mac and load it — this took three non-obvious fixes:

1. **Must be a dylib, not an executable.** Modern ld64 always emits `LC_MAIN`
   (`LC_REQ_DYLD`), which 3.2's dyld cannot parse → refuses to load. A dylib has
   no `LC_MAIN`.
2. **Must compile `-marm`.** With default Thumb, the constructor pointer in
   `__mod_init_func` is even (no Thumb bit); 3.2 dyld jumps to it in ARM mode and
   the process dies `EXC_BAD_ACCESS (SIGBUS)` at the init address. ARM code, even
   pointer, no interworking — runs clean.
3. `-miphoneos-version-min=7.0` is fine: the resulting `LC_VERSION_MIN_IPHONEOS`
   is not `LC_REQ_DYLD`, so 3.2 dyld ignores it. (3.2 SDK link target is rejected
   by ld64 outright — "3.2 minimum deployment target is no longer supported".)

Build line:
```
xcrun clang -arch armv7 -marm -miphoneos-version-min=7.0 -isysroot <3.2.sdk> \
  -dynamiclib -install_name /var/root/kmem.dylib -o kmem.dylib kmem.c
ldid -S kmem.dylib
```

**Injection vector: `DYLD_INSERT_LIBRARIES`, NOT cynject.** MobileSubstrate is
broken on this device — `cynject <pid> dylib` fails with
`MSHookProcess() failed` / `_krncall(mach_vm_deallocate...)` mach errors for any
target, even a trivial dylib. Instead load into a throwaway `sleep`:
```
DYLD_INSERT_LIBRARIES=/var/root/kmem.dylib sleep 0
```
The dylib constructor reads its command from `/var/root/kmem.cmd` and writes the
result to `/var/root/kmem.out`. `kread.sh` wraps the whole round-trip from the Mac:
```
./kread.sh 'r c02787b8 16'   # hexdump 16 bytes at kernel VA 0xc02787b8
./kread.sh 'regions'         # walk the target task's VM map
./kread.sh 'tasks'           # list pset tasks with pid + first region
```

## What does NOT work — no userland path to kernel/physical memory on this JB

The whole plan hinges on a kernel task port. On this device there isn't one:

- `task_for_pid(mach_task_self(), 0, &kt)` → **KERN_FAILURE** (blocked).
- `processor_set_tasks()` on the privileged pset **succeeds** and returns **25
  tasks**, but **none has pid 0** — `kernel_task` is not in the list. `tasks[0]`
  is an ordinary userspace task (its VM map shows dyld at 0x2fe00000 and the
  armv7 shared cache at 0x30000000, i.e. a user process, not the kernel).

So `vm_read` can only reach *userland* maps. Kernel VAs (0xc0000000+, e.g.
`debug_enabled` at 0xc02787b8) return `KERN_INVALID_ADDRESS`, and MMIO
(0xBF100000 PMGR, 0xBF102000 timer, 0xBFA00000 GPIO, 0xBF200000 VIC) is
unreachable — those are pmap-level static mappings with no `vm_map_entry`, and we
have no kernel map anyway. The DRAM-mirror test (compare phys 0x40000000 /
0x50000000 / 0xC0000000) also needs physical reads → not possible from userland.

**Conclusion:** arbitrary physical/MMIO reads from the *running* 7B500 userland
are not available with this jailbreak. The remaining routes are:

- **HW-1 iBEC `md` path** (already built in `contrib/ipad1-hw/`) — the reliable
  register oracle, but it needs DFU/reboot.
- **A kernel patch or exploit** to expose `kernel_task` / a `/dev/kmem`, then
  `kmem.dylib` reads everything as-is (the tool is ready the moment a kernel task
  port exists — point `kernel_task()` at it).
- **IOKit from userland** (works without a kernel task): the IORegistry gives the
  device-tree `reg` MMIO windows, published clock rates, timebase, DRAM layout and
  chip revision. This is the route that produced the results below.

## What we measured (IOKit route — no kernel task needed)

`iokit.dylib` walks the IORegistry (`IORegistryEntryCreateCFProperties` over the
IOService plane) and dumps every entry's properties. CF/IOKit are resolved via
`dlopen`/`dlsym` because the 3.2 SDK stubs won't link under modern ld64 ("built
for unknown"). Run it the same way as `kmem.dylib`:
```
DYLD_INSERT_LIBRARIES=/var/root/iokit.dylib sleep 0   # writes /var/root/kmem.out
```
Results are decoded in `../regs/`:
- `ioreg-full.txt` — all 285 IORegistry entries (raw).
- `mmio-windows.txt` — decoded physical MMIO windows: PMGR 0xBF100000 (+6 sub-
  blocks), VIC 0xBF200000, GPIO 0xBFA00000, etc., plus the arm-io child→phys
  mapping (child 0x3Fxxxxxx + 0x80000000). Confirms the plan's addresses. Timer
  0xBF102000 lives inside the PMGR window (no separate node).
- `clocks-and-gates.txt` — timebase 24 MHz, bus/peripheral 100 MHz, and the full
  device→PMGR clock-gate-index table.
- `hardware-facts.txt` — 256 MiB DRAM @ 0x40000000, chip-revision 0x11, caches, etc.

**RAM mirror:** cannot be *proven* from userland (no physical read), but there is
strong indirect evidence the 0x50000000 mirror exists — `pram` is declared at
physical 0x5FFFC000 while DRAM is only 256 MiB based at 0x40000000, so 0x5FFFC000
is real RAM only if 0x50000000 aliases 0x40000000. No evidence for a 0xC0000000
mirror. A definitive test needs the HW-1 iBEC `md` path. See `mmio-windows.txt`.

## Files
- `kmem.c` / `kmem.dylib` — the reader (ops: `r <va> <len>`, `regions`, `tasks`).
- `kread.sh` — Mac-side driver over `ipad-ssh.sh`.
- `probe.c` — minimal constructor dylib used to pin down the load requirements.
