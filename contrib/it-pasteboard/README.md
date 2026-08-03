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
* `it_pbd.c` — the real thing: a launchd-started daemon that carries text both
  ways over the `QC_PB_*` ops. `it_pbd` is the built armv6 binary, tracked so
  an image can be built without the toolchain.
* `com.qemu.it-pbd.plist` — its LaunchDaemon.
* `build.sh` — armv6 build, see `../armv6-toolchain/README.md`. Plain C with a
  dlopen'd ObjC runtime, because `ld -lobjc` against the 3.1.3 SDK is fatal.

## Installing it

    scp it_pbd            root@device:/usr/local/bin/it_pbd
    scp com.qemu.it-pbd.plist \
        root@device:/System/Library/LaunchDaemons/com.qemu.it-pbd.plist
    ssh root@device 'chmod 755 /usr/local/bin/it_pbd
                     chown root:wheel /System/Library/LaunchDaemons/com.qemu.it-pbd.plist
                     launchctl load /System/Library/LaunchDaemons/com.qemu.it-pbd.plist'

`chown root:wheel` is not decoration: **launchd silently ignores a plist it does
not see as root-owned**, and `imgtools/editimg.py` creates files as uid 501. The
daemon logs to `/var/log/it_pbd.log`, one line per item in either direction,
which is the fastest way to tell "the host never sent it" from "the guest never
took it".

## Driving it from the host

    # headless / scripted
    qom-set path=/machine property=pasteboard value="Hello. World 42 #tag"
    qom-get path=/machine property=guest-pasteboard

    # in a window
    Edit > Paste Text to Guest   (Cmd+Ctrl+V, as in the iPhone Simulator)

Both go through the same machine property, so there is one implementation, not
two. The Cocoa item is the only part that needs a clipboard peer; a headless run
has none, which is why the QMP path is not the afterthought it looks like.

## The host half

`QC_PB_POLL/READ/ACK` and `QC_PB_WRITE/COMMIT` in
`include/hw/arm/guest-services/general.h`, on the existing cp15 `QEMU_CALL`
tunnel — the same mechanism `QC_POLL_INPUT` uses, and reachable from PL0, so no
kernel patch is involved. The text is windowed through a guest buffer rather
than carried in the args union: the 52-byte `qemu_call_t` layout is frozen
(`contrib/it-kbd-agent` is compiled into NAND images we cannot rebuild and
hardcodes it), so any new args struct has to stay at or under 32 bytes.
