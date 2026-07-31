# it_kbd_agent — host-keyboard text input for the emulator

Lets you type in the emulator's display window and have the text land in the
focused text field, with **no on-screen keyboard**.

## How it fits together

Two halves:

1. **QEMU side (done, in-tree).** `hw/arm/ipod_touch_2g.c` captures host key
   presses in the display window via the modern input handler. The four hardware
   buttons move behind the host **Command** modifier so every plain key is free
   for text:
   - `Command+L` → power
   - `Command+Shift+H` → home
   - `Command+Minus` → volume down
   - `Command+Equal` → volume up

   Bare printable keys are converted (honouring Shift) to unichars and queued in
   a per-machine ring. `QC_POLL_INPUT` (0x130) on the cp15 `QEMU_CALL` tunnel
   (`hw/arm/guest-services.c`) dequeues one unichar. `IT_KBD_TRACE=1` logs each
   queued char.

2. **Guest side (this agent).** `it_kbd_agent.c` is injected into SpringBoard,
   polls `QC_POLL_INPUT` on the main run loop, and feeds each unichar to
   GraphicsServices' `_GSPostSyntheticKeyEvent` (@ `0x31553684` on 5F138) — the
   same text-input path the on-screen keyboard drives.

## Status

- The agent **source compiles clean** as armv6 (`clang -arch armv6 -c` against
  `iPhoneOS2.0.sdk`).
- **Blocker: the link.** Modern `ld` refuses armv6 ("linking for armv6 is no
  longer supported"; `-ld_classic` is also gone). The 2G's ARM1176 is armv6-only,
  so the dylib must be armv6. You need an **armv6-capable linker**:
  - a cctools/ld64 port built with armv6 support (e.g. tpoechtrager/cctools-port),
    or
  - the `ld64`/`gcc-4.2` host toolchain extracted from the Xcode 3.x installer
    packages (`/Volumes/iPhone SDK/Packages`), or
  - an existing armv6 jailbreak build environment (theos with an old toolchain).

  Once linked, `build.sh` ad-hoc signs the dylib. The emulator image already runs
  injected armv6 binaries (that is how decrypted apps run), so a correctly linked
  armv6 dylib will load.

## Inject + verify

1. Build `it_kbd_agent.dylib` (needs the armv6 linker above).
2. Inject offline: place the dylib in the NAND image and set
   `DYLD_INSERT_LIBRARIES=/path/to/it_kbd_agent.dylib` in SpringBoard's launchd
   plist via `imgtools/patch_launchd_env.py` (same route as other tweaks), with
   `nandrw=` so it persists.
3. Boot with `-display sdl`, open Notes/Spotlight, type on the host keyboard.
   Text should appear with no OSK; `Command+Shift+H` still returns home.

Verification needs the SDL window and a human at the keyboard — it cannot be
checked headless.
