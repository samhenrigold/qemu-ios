# Getting text into the guest without tapping the on-screen keyboard

Typing by synthesising taps on iOS's own keyboard is structurally lossy: key
positions change per keyboard page and per field type (a URL field replaces the
space bar), there is no feedback channel, and the page state can desynchronise
silently — after which every subsequent character lands somewhere else.

iOS 3.0 added `UIPasteboard`, which is the way out. Put the text on the guest's
general pasteboard and the user taps **Paste**. No geometry, no page state.

## What backs the pasteboard on 3.1.3 (measured, on the device)

Not per-process state, and not a file the host can simply edit:

* `/System/Library/LaunchDaemons/com.apple.UIKit.pasteboardd.plist` launches
  `/System/Library/Frameworks/UIKit.framework/Support/pasteboardd` **on demand**
  (MachService `com.apple.UIKit.pasteboardd`, `UserName mobile`, `KeepAlive`
  with a `TimeOut`). That daemon owns the live state.
* It persists to `/var/mobile/Library/Caches/com.apple.UIKit.pboard/pasteboardDB`
  — its own backing store, created the first time a client connects. Writing
  that file offline is not a channel: the running daemon holds the state, and it
  is the daemon that the rest of the system asks.

So the pasteboard is reachable only by talking to the daemon, and `UIPasteboard`
is its only client. A guest-side process is required; there is no host-only
route.

## It works from ANY process — the pasteboard is not app-private

`pbset` is a plain armv6 command-line binary (no bundle, no `UIApplication`,
run over ssh as root) that does exactly one thing:

    [[UIPasteboard generalPasteboard] setValue:text forPasteboardType:@"public.utf8-plain-text"]

After it ran, a long-press in SpringBoard's Spotlight field raised the **Paste**
menu and pasting inserted the text verbatim — including a period, spaces and a
`#` from the symbols page, the three things the synthesised-tap path gets wrong.
That is the whole feasibility question answered: seed the pasteboard from
outside, and the guest's own UI pastes it.

## THE TRAP: an empty type string kills pasteboardd

**`-[UIPasteboard setString:]` crashed pasteboardd**, every time, with
`EXC_BAD_ACCESS (SIGBUS)`, `KERN_PROTECTION_FAILURE at 0x00000004` — a write
through a NULL CF object. So did `-[UIPasteboard string]` and `pasteboardTypes`
from the same kind of process.

The reason is in pasteboardd's own MIG routines. Each one converts the incoming
pasteboard name and type from C strings, and the conversion is:

    ldrsb r0, [r1]        ; first byte of the type
    cmp   r0, #0
    beq   skip            ; empty -> r0 stays 0 -> the CFString is NULL
    bl    _CFStringCreateWithCString
    ...
    bl    _CFDictionarySetValue   ; NULL key, no check -> crash in CF

An empty type is turned into a NULL key and handed straight to
`CFDictionarySetValue` / `CFDictionaryGetValue`. It is a genuine bug in Apple's
daemon that a real app never trips, because a real app's UTI is never empty.
Something about resolving the UTI in a bundle-less process leaves it empty.

Two consequences worth knowing before debugging anything here:

1. **Always pass the UTI explicitly** (`setValue:forPasteboardType:` with
   `public.utf8-plain-text`). That path stores the data, writes `pasteboardDB`,
   and reads back byte-identical.
2. **A crash wedges the service.** launchd then repeats
   `Check-in of Mach service failed. Already active: com.apple.UIKit.pasteboardd`
   and every later pasteboard call quietly returns nil — which reads exactly
   like "the pasteboard is empty" and will send you chasing the wrong bug.
   Recover without rebooting:

       launchctl unload /System/Library/LaunchDaemons/com.apple.UIKit.pasteboardd.plist
       launchctl load   /System/Library/LaunchDaemons/com.apple.UIKit.pasteboardd.plist

## What is here

* `pbprobe.c` — the feasibility probe: `generalPasteboard`, `setString:`,
  read back. Kept because its FAILURE is the finding: `setString:` is the call
  that crashes the daemon.
* `pbset.c` — the working version: reads `/tmp/pbtext` and sets it with an
  explicit UTI, no reads at all (the reads are what send an empty type).
* `build.sh` — armv6 build, see `../armv6-toolchain/README.md`. Plain C with a
  dlopen'd ObjC runtime, because `ld -lobjc` against the 3.1.3 SDK is fatal.

## What is NOT here yet

The host half. The intended shape is a `QC_PB_*` pair on the existing cp15
`QEMU_CALL` tunnel (`include/hw/arm/guest-services/general.h`) — the same
mechanism `QC_POLL_INPUT` already uses, and reachable from PL0 — with a small
launchd-started guest agent polling it and calling `setValue:forPasteboardType:`,
fed from QEMU's Cocoa clipboard peer (`ui/cocoa.m`,
`QEMU_CLIPBOARD_TYPE_TEXT`). Note the frozen 52-byte `qemu_call_t` layout: any
new args struct must stay at or under 32 bytes.
