# The App Store download UI on iOS 3.1.3, and how much of it an outside process can drive

Goal: when an `.ipa` is dropped on the QEMU window, show what a real App Store
download shows — a placeholder icon on the home screen straight away, with a
progress bar that fills as the install runs, replaced by the real icon at the
end.

**Shipped: the placeholder, end to end.** Drop an `.ipa` on the QEMU window and
a placeholder appears on the home screen while the install runs, then the real
icon takes its place — `DELIVERABLE-2-placeholder-during-install.png` and
`DELIVERABLE-3-real-icon-after-install.png` are two frames of one Epicurious
install, the same slot before and after. No injection and no entitlement.

**Not shipped: the filling progress bar.** It is fed only by an `ISDownload`
object that arrives from `itunesstored` over a launchd-owned Mach service, and
the only ways in are in-process. The bar is drawn but stays empty and the label
is SpringBoard's own "Waiting…", which is what iOS 3 really shows — so nothing
here is faked. The rest of this file is the evidence and the routes left.

## Using it

Nothing new to invoke: `imgtools/install-ipa.sh` does it. That script is what
the Cocoa window runs when an `.ipa` is dropped on it *and* what you run from a
terminal, so a headless install behaves exactly like a drop and the two cannot
drift. The placeholder goes up before `ideviceinstaller` starts and comes down
when it finishes **whether or not it succeeded**; Ctrl-C takes it down too.
Both directions were tested against a live guest, by page-indicator dot.

By hand, on the device:

    sbdlicon add    <unique-id> [<bundle-id>]
    sbdlicon cancel <unique-id>

## The classes, and where each one lives

All addresses are file offsets in the armv6 slice of 3.1.3 (7E18) SpringBoard.
The 3.1.3 SDK's `System/Library/CoreServices/SpringBoard.app/SpringBoard`
armv6 slice is **byte-identical** to the one on the device's root filesystem, so
static analysis of it is analysis of the real thing — checked with `cmp`, worth
re-checking on any other binary before trusting an address.

| Class | Role |
| --- | --- |
| `SBDownloadingIcon` | the placeholder icon. Subclass of `SBIcon`; `initWithDisplayIdentifier:` always builds an `SBDownloadingProgressBar` subview |
| `SBDownloadingProgressBar` | `setProgress:` takes a float 0..1; `updateFill` redraws |
| `SBDownloadController` | `sharedInstance`, owns an `ISDownloadQueue` and is its delegate |
| `SBIconModel` | `addDownloadingIconForDisplayIdentifier:`, `addNewIconToDesignatedLocation:animate:scrollToList:saveIconState:`, `replaceDownloadingDisplayIdentifiers:withDisplayIdentifiers:` |
| `SBIconController` | `noteDownloadStateChanged`, `setIconToInstall:` |
| `ISDownload` / `ISDownloadStatus` / `ISDownloadMetadata` | `iTunesStore.framework` (private). The download itself |

### How progress is expressed

Not a percentage. `-[SBDownloadingIcon downloadStatusChanged:]` computes

    progress = [[status progress] normalizedCurrentValue] / [[status progress] normalizedMaxValue]

as a float, from two 64-bit integers, and hands it to `setProgress:`. The state
shown under the icon comes from `-[SBDownloadingIcon displayName]`, which is a
localized SpringBoard string chosen by the status, **not** the app's name:

* `[status isFailed]` -> `[download title]` (the only case that shows a name)
* `[status isPaused]` -> `PAUSED_ICON_LABEL`
* no status, or `[status progress] == nil` -> `WAITING_ICON_LABEL`
* `_installing` (set when `[progress operationType] == 1`) -> `INSTALLING_ICON_LABEL`
* otherwise -> `DOWNLOADING_ICON_LABEL`

So "waiting / downloading / installing / paused / failed" are all already there
and need no invention; the app's own name never appears on the placeholder.
Artwork comes from the download (`-[ISDownload loadArtworkImage]` ->
`-[SBDownloadingIcon download:loadedArtworkImage:]`), so an icon with no
download draws the generic dark placeholder.

## What normally drives it: a distributed notification, from one specific process

`-[SBDownloadController downloadQueue:changedWithRemovals:disappearances:]` is
the whole placement path. For each download in the queue it does

    [download loadArtworkImage]
    displayID = +[SBDownloadingIcon displayIdentifierForDownload:download]
    existing  = [iconModel iconForDisplayIdentifier:displayID]
    icon      = [iconModel addDownloadingIconForDisplayIdentifier:displayID]
    [icon setDownload:download]
    if (!existing && ![displayID isEqual:iconController.iconToInstall.displayIdentifier])
        [iconModel addNewIconToDesignatedLocation:icon animate:… scrollToList:… saveIconState:…]

and on removal `replaceDownloadingDisplayIdentifiers:withDisplayIdentifiers:`
followed by `-[SBApplicationController loadApplicationsAndIcons:reveal:popIn:]`
— which is exactly the "the real icon replaces the placeholder" step.

That delegate is called by `ISDownloadQueue`, which is a **client** of a
`CPDistributedNotificationCenter` (AppSupport) named
`com.apple.iTunesStore.daemon-notifications`. The protocol is fully specified:

| Notification | userInfo |
| --- | --- |
| `ISNotificationDownloadsAdded` | `param` = keyed archive of `NSArray<ISDownload>`, `indexSet` = keyed archive of `NSIndexSet` |
| `ISNotificationDownloadStatusChanged` | `item-id` = `NSNumber` (`unsignedLongLongValue`), `param` = keyed archive of `ISDownloadStatus` |
| `ISNotificationDownloadsChanged` | `param` = keyed archive of the downloads |
| `ISNotificationDownloadsReplaced` | `indexSet` + `param` |
| `ISNotificationDownloadsRemoved` | `param` (read with plain `objectForKey:`) |

(The keys are the exported constants `ISParameterKey` = `@"param"`,
`ISIndexSetParameterKey` = `@"indexSet"`, `ISItemIdentifierParameterKey` =
`@"item-id"`; values go through `ISGetUnarchivedParameter`, i.e.
`NSKeyedArchiver`.)

### Why an outside process cannot post them

`-[CPDistributedNotificationCenter postNotificationName:userInfo:]` raises
**"Must be running %@ '%@' server to send post notifications"** unless the
caller is the center's *server*. A client's only outbound call is a check-in.

Becoming the server means owning the Mach name. `runServerOnCurrentThread`
tries `bootstrap_check_in` first and falls back to
`mach_port_allocate` + `bootstrap_register2` — so on **2.1.1** the name is
free (its `com.apple.itunesstored.plist` declares only
`com.apple.iTunesStore.daemon`) and anyone could claim it. On **3.1.3 the plist
declares it**, alongside `.daemon`, `.daemon.public` and
`.daemon.notifications.public`, so launchd holds the receive right and hands it
only to the job labelled `com.apple.itunesstored`. `bootstrap_register2` on a
launchd-declared name fails. **That is the blocker.**

Useful detail if this is ever picked up: the server posts the Darwin
notification `CPDistributedNotificationCenterDidRestartNotification-<name>` when
it starts, and clients re-check-in on it — so a server that appears *after*
SpringBoard has booted is still picked up. Nothing here depends on ordering.

## What an outside process CAN do, and does

SpringBoardServices exports MIG stubs onto SpringBoard's own server port, which
`SBSSpringBoardServerPort()` hands to any process:

    int SBAddDownloadingIconForDisplayIdentifier(mach_port_t, const char *uniqueID,
                                                 const char *bundleID);
    int SBCancelDownloadingIconForDisplayIdentifier(mach_port_t, const char *uniqueID);

`sbdlicon` here is a 6 KB armv6 binary that calls them. SpringBoard's handler
(`sub_42e6c`) turns `uniqueID` into a display identifier via
`+[SBDownloadingIcon displayIdentifierForDownloadUniqueID:]`, creates the icon,
sets its bundle ID, and then:

* if `bundleID` is **already installed** -> `addNewIconToDesignatedLocation:`,
  and the placeholder appears immediately, in that app's slot. Measured:
  `sbdlicon add dl-demo-2 com.apple.mobilenotes` replaced the Notes icon with a
  dark placeholder labelled "Waiting…"
  (`DELIVERABLE-1-placeholder-over-an-installed-app.png`).
* if it is **not installed** -> only `[SBIconController setIconToInstall:]`,
  which just stashes the icon in an ivar. Measured: nothing appears.

The second case is the one that matters when a new `.ipa` is dropped, so for a
while it looked as though this RPC was not the feature after all. **The way
round is the ordering inside the handler**: the icon is created and registered
in `SBIconModel`'s dictionary under `com.apple.downloadingicon-<uid>` *before*
`iconForDisplayIdentifier:` runs. Pass that same string as the bundle id and the
lookup finds the icon that was just made, so the "already installed" branch is
taken and the icon is placed after the last one on the home screen. Measured: it
appears, and `cancel` removes it and collapses the page again.

That is what `add` does when no bundle id is given — it is what a caller
installing a new app always wants, and putting the trick in `sbdlicon` rather
than in a shell quoting expression keeps it explained in one place. Pass a
bundle id explicitly only for the update case.

Two properties worth knowing, both of which make the failure path cheap:
placement uses `saveIconState:NO`, so a placeholder is never written to disk and
cannot outlive the running SpringBoard; and `addDownloadingIconForDisplayIdentifier:`
is idempotent per display identifier, so keying the id on the bundle id means
dropping the same `.ipa` twice reuses one icon instead of stacking them.

`sbunlock` is here because headless verification needs a way past the lock
screen that is not synthesised touch: it calls
`SBApplicationRequestedDeviceUnlock`. Note it raises the passcode keypad even
with no passcode set — tap Cancel, then slide.

## The two routes left for real progress, and which to take

Both require code inside a process that holds the notification server's receive
right, i.e. inside `itunesstored`. Neither touches SpringBoard, so neither can
wedge the boot the way an injected SpringBoard dylib can — if the code crashes,
launchd restarts `itunesstored` and the UI is unharmed.

1. **`DYLD_INSERT_LIBRARIES` in `com.apple.itunesstored.plist`.** The real
   daemon still runs and still owns everything; our dylib posts through
   `ISGetDistributedNotificationCenter()`, which in that process *is* the
   server, so `postNotificationName:userInfo:` is legal. Smallest change, and
   the store keeps working.
2. **Move the MachService.** Delete
   `com.apple.iTunesStore.daemon-notifications` from `itunesstored`'s plist and
   declare it in our own LaunchDaemon; launchd then hands *us* the receive
   right. Cleaner process boundary, but it takes the notification path away
   from the real daemon.

Route 1 is the one to build. The `ISDownload` objects should be constructed by
linking `iTunesStore.framework` rather than by hand-rolling the keyed archives —
the archive format then never has to be reverse-engineered.

Progress source: `ideviceinstaller` prints real percentages on stdout, and
`ISDownloadStatus`'s progress is a current/max integer pair, so the host figure
maps onto it directly with no interpolation. `[progress operationType] == 1`
is what flips the label to "Installing…".

## Building

    ./build.sh          # needs ../armv6-toolchain

Note the explicit `_start`: `../armv6-toolchain` rewrites `LC_MAIN` as the
`LC_UNIXTHREAD` that 2010 dyld understands, with the pc pointing straight at the
entry symbol and no crt1 in the way. Nothing sets up `argc`/`argv`, so a plain
`main(int, char **)` reads whatever is in r0/r1 and takes a SIGBUS at the first
`argv[]` access — before any output, which reads as "the binary is broken".
