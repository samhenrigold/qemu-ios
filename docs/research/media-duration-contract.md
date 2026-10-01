# Native MusicLibrary duration input (7E18)

The purchase-folder bridge accepted a finite positive `duration_ms` NSNumber
and passed it directly to stock MusicLibrary. Converted ADTS AAC exposes a
fractional AVFoundation duration; a production import kept all tags and artwork
but stored `item.total_time_ms=0`.

A controlled native test staged identical converted AAC samples under four
separate paths and varied only title/duration metadata. After stock import and
post-sync processing, SQLite readback was:

| Input duration_ms | Stored total_time_ms |
| --- | --- |
| integer 6000 | 6000 |
| real 6000.0 | 6000 |
| real 6060.408163265306 | 0 |
| integer 6060 | 6060 |

Evidence: `/private/tmp/ltm-aac-duration-contract2/`. This is a measured purchase
API input contract, not an audio decoder or storage-model repair.

The bridge now rounds once to positive whole milliseconds at the native API
boundary, retaining its finite/range validation. It applies to both songs and
movies. Neither host library files nor guest database duration columns are
patched. The payload advances to serial 12 / 1.1.10 so the corrected helper is
part of the distributed candidate; media commits also upload the current helper
rather than trusting a baked older copy.

Production Swift preflight, AFC upload, guest import and reconciliation pass for
a fully tagged ADTS AAC file. The stock library reports 6060 ms, all twelve tags
and all native artwork formats, including the checked 320×320 cover pixels.
The guest confirms shutdown. Evidence:
`/private/tmp/ltm-aac-production-native-final.log` and its recorded OUTPUT folder.
The independent native hard-stop/cold-reopen gate now requires exact rounded
duration alongside tags, factory identity and stock MediaPlayer-decoded artwork:
`/private/tmp/ltm-aac-duration-native-cold/` (3065 ms for its three-second fixture).

The host tag-loss fix is separate: Light Touch reads tags/cover and source-based
content identity from its immutable AAC copy before sample conversion discards
those tags. MP3/M4A source bytes are still uploaded unchanged. The offline
metadata check exercises both changes with MP3, M4A and real tagged ADTS AAC;
different tags on identical AAC samples produce distinct import candidates.
Older stripped or zero-duration library items are not silently rewritten.
Native qualification here is 7E18; other MusicLibrary API generations still need
separate version adapters and native proof.
