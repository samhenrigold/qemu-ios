# Building armv6 Mach-O for the guest, without a Snow Leopard box

The retired `contrib/it-kbd-agent/build.sh` documented the state of the art here as: keep a
Mac OS X 10.6.8 machine with Xcode 4.2 around, and `ssh` to it. That works, but
it makes every guest-side binary in this project depend on a second computer.

It is not necessary. A stock modern macOS toolchain can produce armv6 Mach-O
that iOS 3.1.3's dyld loads and its ARM1176 executes. `armv6.sh` does it.

## What is actually blocked, and what only looks blocked

The dead end is **the linker, and only the linker**. `clang` still compiles
armv6 without complaint. `ld` refuses:

    ld: linking for armv6 is no longer supported

`-ld_classic` does not bring it back; the flag is accepted and ignored.

Everything else that fails does so for a different reason and needs a different
fix, which is worth keeping straight because the symptoms overlap:

| symptom | actual cause | fix |
| --- | --- | --- |
| `ld: linking for armv6 is no longer supported` | linker arch support | link as armv7, patch the subtype back |
| `ld: dynamic executables or dylibs must link with libSystem.dylib` | the modern iOS SDK has no armv7 slice | link against the 3.1.3 SDK's own fat libSystem stub |
| binary is rejected or crashes instantly on the guest | `LC_MAIN`, which postdates this OS by two years | rewrite it as `LC_UNIXTHREAD` |
| illegal instruction on the guest | clang defaulted to Thumb-2 | `-marm` |

## The pipeline

1. `clang -target armv6-apple-ios5.0 -marm` — armv6-legal ARM-mode code. `-marm`
   is load-bearing: the default for this target is Thumb, and clang will happily
   emit Thumb-2, which the ARM1176 cannot execute. Targeting armv6 rather than
   armv7 also keeps `movw`/`movt` out of the output.
2. `subtype.py <obj> 9` — rewrite the object's cpusubtype to armv7, because `ld`
   will not accept `-arch armv6` at all.
3. `ld -arch armv7 -syslibroot <3.1.3 SDK> -lSystem` — the 3.1.3 SDK ships a fat
   `libSystem.B.dylib` with armv5/armv6/armv7 slices, so there is a real armv7
   stub to link against. The modern iOS SDK dropped armv7 and has nothing.
4. `mkold.py <out>` — undo the parts of the output that postdate 2010:
   * strip `LC_VERSION_MIN_IPHONEOS`, `LC_SOURCE_VERSION`, `LC_ENCRYPTION_INFO`,
     `LC_UUID`, `LC_FUNCTION_STARTS`, `LC_DATA_IN_CODE`, `LC_BUILD_VERSION`
   * rewrite `LC_MAIN` (iOS 6) as the `LC_UNIXTHREAD` it would have been, with
     `pc` pointing at `_main`
   * put cpusubtype back to 6

`LC_DYLD_INFO_ONLY` is deliberately **not** stripped: the stock 3.1.3
`MBXGLEngine.bundle` uses it, so this dyld handles compressed linkedit fine.

Removing load commands only shrinks the header, so every file offset in the
binary stays valid and no fixups are needed. `LC_MAIN` -> `LC_UNIXTHREAD` grows
by 60 bytes, but the commands dropped alongside it free more than that.

### One consequence of LC_UNIXTHREAD

There is no crt1. `pc` lands directly on `_main`, so `argc`/`argv` are not set up
and `lr` is whatever the kernel left behind — **`main` must not return**. Call
`exit()` (or `_exit()`) explicitly.

LEGACY_LINK=1 executables are the exception: link6 links `crt1old.c` and enters at its `_start`,
which does what iPhone OS 1.x's crt1 did (1.x libSystem does not initialize itself: without
`*_cthread_init_routine` the first `snprintf` dereferences NULL) and then `exit(main(...))`. cc6
also force-includes `legacy.h` for that code: 1.x's `stat`/`readdir` are the 32-bit-inode ABI, the
SDKs' are the 64-bit one, so both go through 1.x's `stat64` family / a converted dirent wherever
libSystem exports `stat64` (only 1.x does). One binary runs on 1.x, 2.x and 3.x+.

If you use `_exit()`, remember it does not flush stdio. The first version of the
`dlopen` probe printed nothing at all for exactly that reason and looked like a
silent failure of the thing it was testing. `pl0trap.c` writes with `write(2)`
and sidesteps the question.

## What has actually been run on the guest

On iOS 3.1.3 (7E18), booted, over ssh — all three Mach-O kinds built this way
loaded and executed:

* **bundle** — `dlopen` + `dlsym` + call, correct return value.
* **dylib** — same, and additionally loaded via `DYLD_INSERT_LIBRARIES` into a
  stock Apple-signed `/bin/launchctl`, which then `dlopen`ed the unsigned bundle
  and ran its code.
* **executable** — `pl0trap`, which is in this directory.

The image had `amfi_allow_any_signature=1 cs_enforcement_disable=1` and the
`_MISValidateSignature` patch inside the dyld shared cache (see
`~/Developer/qemu-ios-files/ssh/README.md`). Which of those is load-bearing for
`dlopen` specifically was **not** isolated — it would cost a reboot per variable
and every image we care about already carries all of them.

## pl0trap

`pl0trap.c` answers one question: does an unprivileged process reach `QEMU_CALL`?

`QEMU_CALL` (cp15 opc1=3 crn=15 crm=15 opc2=0) is declared `PL0_RW` in
`hw/arm/ipod_touch_2g.c`, so in principle user mode can trap to the host with a
bare `mcr` and no kernel patch anywhere. That is the foundation of any userspace
shim, so it is worth measuring rather than assuming.

It needs the host side to answer with a magic value, because the two failure
modes are otherwise identical from the guest: an unhandled cp15 write on this
machine is *silently discarded* rather than faulting, so "the host never saw it"
and "the host saw it and did nothing" both leave the struct untouched. The probe
writes a sentinel, issues the `mcr`, and checks that only the host could have
replaced it.

    ARMV6_SDK=/path/to/iPhoneOS3.1.3.sdk ./armv6.sh
    scp -P <port> pl0trap root@127.0.0.1:/tmp/ && ssh ... /tmp/pl0trap

Measured on 3.1.3, from a plain non-root-privileged user-mode process:

    PL0: about to mcr p15,3,r0,c15,c15,0 from user mode
    PL0: mcr returned (so it did not fault)
    PL0: retval=0x6a17c0de
    PL0: RESULT=TRAP_WORKS_FROM_USER_MODE

Requires a QEMU built with `QC_GLES_PING` (see
`include/hw/arm/guest-services/general.h`).

## Linking against a firmware's own frameworks (1.x)

There was no SDK before 2.0, so 1.x code that calls UIKit (or anything past libSystem) links against the
firmware itself: `mktbd.py ROOT /System/Library/Frameworks/UIKit.framework/UIKit UIKit.tbd` writes a
text stub with the binary's install name, versions and exported symbols, declared for armv7 as link6
links. Fragile-ABI classes go in as their `.objc_class_name_X` symbols. Pass the stubs to link6 as
inputs, and point LEGACY_SYSTEM_STUB at a directory holding a `libSystem.tbd` made the same way so
`-lSystem` binds only what that libSystem has. `contrib/hello-2007/build.sh` is the worked example,
Objective-C included (`-fobjc-runtime=macosx-fragile-10.5`). Two 1.x runtime traps it documents: the
delayed library initializers and the fragile ivar layout (docs/m68/sideload.md).
