# Guest services on the iPad 1 (iOS 3.2.2, armv7)

The iPod guest tools, rebuilt for the ipad1 machine. Nothing here has run on an
emulated iPad yet (it does not reach userland); everything below was built and
checked offline.

```
contrib/ipad1-guest/build.sh                 # -> build/ipad1-guest/, ldid-signed
imgtools/ipad1_rootfs.py build ...           # as before
imgtools/ipad1_rootfs.py bake OUT/pristine   # installs the tools into system.img + data.img
imgtools/ipad1_nand.py build ...             # rebuild the store from the baked images
```

**Do not boot a baked image until the host side below exists.** `it_typein.dylib`
is inserted into SpringBoard and issues the cp15 call; on the ipad1 machine that
coprocessor register is not registered, so the `mcr` UNDEFs and SpringBoard dies
with SIGILL in a loop (and `it_agent` crash-loops under KeepAlive).

## Guest tools

| Tool | 3.2.2 | Baked | Notes |
|---|---|---|---|
| `it_agent` (it-agent) | yes | `/usr/local/bin`, `com.qemu.it-agent` job | exec/put/get/launch/frontmost/lockstatus/orientation/kill/halt + clipboard; signed with the SpringBoard launch entitlement |
| `it_typein.dylib` (it-agent) | yes | `/usr/lib`, SpringBoard `DYLD_INSERT_LIBRARIES` | host keyboard text + `uidump` |
| `sblaunch` (it-gles) | yes | `/usr/local/bin` | entitlement-signed |
| `sbdlicon`, `sbunlock` (it-instprogress) | yes | `/usr/local/bin` | |
| `itstatus`, `itorient`, `ithalt`, `itbattery` | yes | no | legacy ssh fallbacks the agent replaced; not baked on the iPod either |
| `MBXGLEngine`, gles tests (it-gles) | **iPod-only** | no | PowerVR MBX engine replacement; the iPad is SGX (see userland-gl-display.md) |
| `it_kbd_agent`, `it_pbd` | superseded | no | replaced by `it_typein` / `it_agent`; the iPod bake retires them too |
| `isprogress.dylib` | not built | no | `contrib/it-instprogress/isprogress.c:265` does not compile at HEAD (comment block broken) — for the iPod too |
| it-proxy, it-webproxy, it-media, it-audio | not ported | no | outside the guest-services layer |

## What changed from the iPod version

- **Toolchain.** `contrib/armv6-toolchain/armv6.sh` takes `GUEST_ARCH=armv7`:
  real armv7 code (`-target armv7-apple-ios5.0 -marm`), no armv6 subtype round
  trip, `mkold.py --subtype 9`, linked against the 3.2 SDK's fat libSystem stub.
  Sources are unchanged; they already `dlopen` everything and never link a
  framework.
- **Executables, not only dylibs.** hw2-regs/README-native-code-on-3.2.2.md
  says 3.2 dyld rejects modern executables because of `LC_MAIN`. `mkold.py`
  already rewrites `LC_MAIN` into the `LC_UNIXTHREAD` a 2010 linker emitted and
  drops `LC_VERSION_MIN`/`LC_BUILD_VERSION`, which is what let the same tools run
  on 3.1.3. `-marm` covers that README's other trap (even `__mod_init_func`
  pointers). **Unverified on 3.2.2 hardware**; the cheapest proof is copying
  `build/ipad1-guest/itstatus` to the real unit and running it over ssh.
- **Signing.** Every output is `ldid -S` (it_agent and sblaunch with
  `it-gles/sblaunch-entitlements.xml`). Ad-hoc signatures need
  `amfi_allow_any_signature=1` even on the pristine base, which otherwise needs
  no AMFI boot-args.
- **Bake.** `ipad1_rootfs.py bake` replaces `bake-guest-tools.sh` + `editimg.py`
  + `setowner.py`: one mount of each raw image, then uid/gid patched in the
  catalog (`build_nand.set_owner`, which now handles 7B500's 8 KiB allocation
  blocks). Tools 0:0 and 0755, the job 0:0 0644, the AFC markers
  (`/var/mobile/Media/.lt-guest-tools-v{1,2}`) 501:501 on the data volume.
  Each tool is checked (armv7, no `LC_MAIN`/`LC_VERSION_MIN`, signed) before
  anything is written.
- **Dropped from the iPod bake:** `CA_ENABLE_OGL=1`/`LK_ENABLE_OGL=1` (the ipad1
  image deliberately sets `CA_ENABLE_OGL=0`), the MBX engine, the Sounds
  defaults and `SBDontLockEver` cleanup (iPod image history).

## Host side needed on the ipad1 machine

1. **Register the channel on the Cortex-A8** in `hw/arm/ipad1.c`: the same
   `QEMU_CALL` ARMCPRegInfo as `ipod_touch_2g.c` (cp15, opc1 3, crn 15, crm 15,
   opc2 0, `PL0_RW`, `ARM_CP_IO`, `ARM_CP_STATE_AA32`, `qemu_call` /
   `qemu_call_status`), via `define_arm_cp_regs(s->cpu, ...)` after the CPU is
   created. Guest encoding: `mcr p15, 3, rX, c15, c15, 0` with a pointer to a
   `qemu_call_t`.
2. **Detach `guest-services.c` from the iPod machine.** Every service case
   casts `qdev_get_machine()` to `IPodTouchMachineState` for `agent`, the
   keyboard ring (`kbd_ring/head/tail`) and the pasteboard fields. Move those
   into a small shared struct both machines embed and hand it to `qemu_call`
   through `ri->opaque`. `QC_GLES` should return `QC_ERR_ENOSYS` on ipad1 (MBX
   engine), not link `guest-gles.c`.
3. **meson:** add `guest-services.c` and `ipod-agent.c` to `CONFIG_IPAD1`.
4. **QOM properties** on the ipad1 machine, mirroring the iPod's so
   `imgtools/itqmp.py agent ...` and the frontend work unchanged:
   `agent-request`, `agent-result`, `agent-cancel`, `agent-status`,
   `pasteboard`/`pasteboard-status`, the keyboard-text property, and
   `ipod_agent_publish`/`ipod_agent_reset` on machine init/reset.
5. **Halt:** the agent's `halt` op only calls `reboot2(RB_HALT)`. The host must
   still see a power-off from the iPad's PMU model before it treats the guest
   as down, as on the iPod.

Unchanged and expected to carry over: `agent_copy` walks the current guest page
tables through `cpu_memory_rw_debug` (single core, `qemu_get_cpu(0)`), and the
agent already pre-touches its receive pages because debug writes cannot fault in
demand-zero pages.

## Offline checks run

- `contrib/ipad1-guest/build.sh`: all nine outputs are thin `arm_v7`, zero
  `LC_MAIN`/`LC_VERSION_MIN`, `LC_CODE_SIGNATURE` present, `mcr p15, #3, rX,
  c15, c15, #0` present in it_agent and it_typein.
- `ipad1_rootfs.py --selfcheck` covers the tool check and the
  `DYLD_INSERT_LIBRARIES` edit.
- `bake` on a clone of `userland/pristine`: fsck clean, owners/modes read back
  from the catalog as listed above, SpringBoard env
  `CA_ENABLE_OGL=0, MBX2D_PAGE_FLIP=0, DYLD_INSERT_LIBRARIES=/usr/lib/it_typein.dylib`.
