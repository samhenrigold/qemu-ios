# Native media import (7E18)

`itmedia` adds one staged song or movie through the guest's own MusicLibrary framework.
It preserves existing songs and lets iOS write its SQLite tables, indexes,
locations, Purchased playlist and backup files. It does not generate a legacy
iTunesDB or rewrite the library on the host.

This changes the original plan's D.1/D.2 implementation choice: iOS 3.1.3 uses
`iTunes_Control/iTunes/iTunes Library.itlp/*.itdb`. The native
`-[MLMusicLibrary_SQL insertItemFromPurchaseFolder:withItemProperties:]` service
was verified on 7E18; the helper rejects other firmware builds. Upstream
[libgpod's SQLite notes](https://github.com/fadingred/libgpod/blob/master/README.sqlite)
describe the format transition. No libgpod implementation is included here.

Build:

```sh
ARMV6_SDK=/path/to/iPhoneOS3.1.3.sdk bash contrib/it-media/build.sh
```

Stage a readable MP3, M4A or WAV for a song, or M4V, MP4 or MOV for a movie, under
`/var/mobile/Media/LightTouch/<staging-id>/<filename>`. The two directories must
be ordinary directories accessible to mobile (uid/gid 501); file mode 0644 is
sufficient. IDs and filenames accept ASCII letters, digits, hyphens,
underscores and dots, up to 128 bytes, with no leading dot. Symlinks are refused.
The metadata plist is limited to 64 KiB:

```xml
<?xml version="1.0" encoding="UTF-8"?>
<plist version="1.0"><dict>
  <key>filename</key><string>audio.m4a</string>
  <key>title</key><string>Example song</string>
  <key>artist</key><string>Example artist</string>
  <key>album</key><string>Example album</string>
  <key>duration_ms</key><integer>6000</integer>
</dict></plist>
```

Run as root (drops to mobile) or mobile:

```sh
/tmp/itmedia /tmp/song.plist staging-id
```

Title, filename and duration are required; artist, album and genre are optional.
The optional `kind` is `song` by default; use `feature-movie` for a movie.
MusicLibrary sets the native media kind and video fields. The caller supplies
metadata and is responsible for validating the codecs; accepting a filename
extension does not mean the device can decode its contents.
Encrypted tracks, transcoding, artwork, playlists other than the native
Purchased list and a Mac movie-import interface are not implemented here.
The separate `itphoto` helper handles photos.

Success ends with `imported` or `already-imported`. The framework may also emit
diagnostics during first-time library creation. A nonzero exit preserves staged
media: a failed or interrupted request can have committed its library change.
Reconcile using the same staging ID and filename, never by blindly choosing a
new ID. Once imported, the staged path is the permanent media location and
must not be removed or overwritten. Imports through this helper serialize on
an advisory lock. Concurrent iTunes synchronization is not supported.

The retry query opens SQLite read-only and matches the file location to an
existing media item, including a movie. It avoids Music's private collation indexes with `NOT INDEXED`,
so a library that has been opened by Music remains queryable after a reboot.
It fails closed on database errors. This is duplicate reconciliation for the
same staged file, not content-based deduplication across different locations.

Native acceptance:

```sh
python3 tests/ipod/test_media_guest.py --base-nand /path/to/nand-agent-v3
```

The test imports generated AAC/MP3 and movie fixtures into a disposable overlay, rejects
malformed requests, reconciles duplicates, checks Music's SQLite records and
the public MediaPlayer song query in Harness, captures the Songs and playback
screens, verifies stereo tones in host audio, and checks persistence and retry
reconciliation after a cold boot. It checks the movie's database classification
and duplicate reconciliation; movie playback is not covered. Compressed decoding is enabled to match Light
Touch. On 7E18 the third-party MediaPlayer query connects to Music's MIG service;
the test starts Music before requesting the Harness count.

## Saved Photos helper

The same build script produces `itphoto`. Stage an upright, 8-bit baseline JPEG
as `/var/mobile/Media/LightTouch/<staging-id>/image.jpg`, then run
`/tmp/itphoto <staging-id>` as root or mobile. Input is limited to 16 MiB and
2048 pixels on each side, checked before UIKit decodes it. Light Touch prepares
this representation on the host. The helper uses 7E18 PhotoLibrary's
`PLCameraAlbum` save operation with `notifyingTargetWithPath:` and waits up to
40 seconds for its callback. The local 3.1.3 SDK's Objective-C metadata confirms
the selector and argument layout; the helper checks its availability at runtime.
iOS creates the DCIM original, poster image and BTH/THM thumbnails.

Before saving, the helper durably writes a `pending` receipt. A successful
callback records `done` and the saved DCIM original path, then removes the redundant staged JPEG; Photos
owns the registered copy. A repeated completed request returns `already-imported`
while that original exists. If the original or its album was deleted, the same
staging ID may import again. DCIM must still be an ordinary directory; malformed
paths, unexpected file types and inspection errors preserve the receipt and
refuse replay. The album and original are checked without following symlinks.
Legacy `done` receipts without a path keep their previous duplicate-only behavior.
A pending or malformed receipt refuses replay, because a killed process may
have saved the photo without recording completion. It preserves the staged
image for inspection. Do not delete receipts to force a retry without checking
Saved Photos first. Different staging IDs are independent imports.

`tests/ipod/test_photo_guest.py` checks registration, thumbnail files, original
dimensions/colors, completed and uncertain receipts, malformed input, and cold
persistence with two guest-confirmed shutdowns. It captures album/grid/full-size
screens and checks grid colors. The new saved-path deletion/reimport behavior
has offline coverage; this cleanup did not run the native guest suite.

`python3 tests/ipod/test_media_helpers.py` runs offline fixture checks for
completed, legacy, uncertain and malformed receipts, deleted originals/albums,
invalid storage entries, and song/movie identity queries. It compiles the
production receipt preflight and stops before calling native Photos, so it
does not validate the framework callback or image decoding.

Before confirming an import, the helper synchronously calls 7E18 ITSync's
`ITDBPrepServerPostProcessRun(NULL, 1)`. The second argument selects
`sendMessageAndReceiveReplyName:userInfo:` rather than the asynchronous send.
This finishes native sorting/index work before the user opens Music.

Without this step, Music's `SyncHelper` starts post-processing during launch
and can exit as its sync phase ends. An isolated interposition trace identifies
`-[SyncHelper _delayedTerminate]` at `0x57bc4`, calling UIKit's
`terminateWithSuccess` with status 0. It is not a volume-button crash. The native
Light Touch media test now requires Music to stay foreground through its first
Songs tab and volume setup; it no longer relaunches Music to recover.

Light Touch identifies newly prepared photos by the SHA-256-derived UUID of the
baseline JPEG, using the same identity helper as music. Re-preparing unchanged
content therefore reaches the same receipt rather than creating a duplicate.
Completed retries remove a restaged JPEG. Existing random-ID imports are not
retroactively indexed. New receipts reconcile deletion by the native DCIM path;
this is not a general bidirectional photo-library index. A path reused for a
different asset cannot be distinguished by these receipts.
