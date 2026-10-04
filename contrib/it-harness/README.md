# Test Harness — iPod touch 2G, iOS 3.1.3

Build an ARMv6 app and installable IPA with the existing legacy SDK toolchain:

```sh
bash contrib/it-harness/build.sh
imgtools/install-ipa.sh --check contrib/it-harness/build/Harness.ipa
```

Requires the SDK configured in `contrib/armv6-toolchain/armv6.sh` (override with
`ARMV6_SDK`), Xcode command-line tools, Python 3, `ldid`, and FFmpeg with libx264
and libmp3lame. No downloaded or copyrighted media: all six fixtures are generated.

Drop `contrib/it-harness/build/Harness.ipa` into LightTouch's app installation UI.
Alternatively, with an AppSync-enabled emulator running:

```sh
imgtools/install-ipa.sh contrib/it-harness/build/Harness.ipa
```

Use a prepared emulator with the GLES bridge installed. This app opens OpenGLES
dynamically, like GLTest, so the command-line installer's linked-framework scan
does not install the bridge automatically. The normal LightTouch package supplies
the bridge. Standalone media decoding requires the switches documented in
`docs/research/ipod-media.md`; unavailable device functions should produce failures or
missing output, not be assumed supported because the app installed.

The scrollable menu provides:

| Test | Evidence |
| --- | --- |
| GLES 1.1 | Real CAEAGLLayer/framebuffer, cyan/magenta background, rotating white triangle, pixel readback, API errors and presentation checks, frame rate |
| Core Animation | Moving and fading orange square; check smoothness visually |
| Storage | 1 MiB deterministic pattern, flush/fsync, atomic rename, full read/byte comparison; separate verification after reboot |
| CPU/memory | 4 MiB allocation/pattern comparison plus known floating-point sum |
| Network | Asynchronous HTTP(S) GET, status/byte count/timing, 1 MiB response cap, timeout and error reporting |
| Audio | Six-second stereo PCM, AAC, MP3 and ALAC; left 440 Hz/right 880 Hz, pause/resume and volume controls |
| Video | Six-second H.264 baseline and MPEG-4 Part 2, moving pattern plus AAC sound; native seek/pause/Done controls |
| Input | Editable text field, keyboard return, buttons and scrolling |
| Tilt | Live accelerometer values and callback detection |
| Persistence/lifecycle | Launch counter in defaults; active/inactive/termination events; saved storage marker |
| Clipboard | String round trip in Run automatic checks |

Stop / Back stops playback, cancels networking, removes animations and returns
to the menu. Audio controls work while audio is playing. The top field is a URL
for networking or a note for Record manual PASS/FAIL. Visual tests require Back
before recording their result. Automatic checks cover storage, CPU/memory and
clipboard; they do not declare visual or audible output correct.

For a local network target, run `python3 -m http.server 8000 --bind 0.0.0.0`
in a directory containing only test data, enable guest networking, and use
`http://10.0.2.2:8000/` (the default). Change the URL for other endpoints.
HTTPS uses the guest's old TLS/trust store; failures are recorded without
bypassing certificate validation. No external service is contacted automatically.

Results append to the app's `Documents/results.log` and are shown on screen.
The full path is logged at launch. Retrieve via guest SSH or the app container
tools. A stored marker is checked at launch without overwriting it; to test
persistence, write it, cleanly shut down/reboot, then verify it again.
The clipboard check restores the previous pasteboard items.

“Media library: count songs” queries `MPMediaQuery songsQuery` and reports the
count and first title. A missing response is reported as an unavailable service,
not as zero songs. On 7E18 the service is hosted by Music; start Music before
running this check. `tests/ipod/test_media_guest.py` drives it with generated
AAC/MP3 imports in a disposable overlay.

The host check validates packaging, not runtime behavior:

```sh
python3 contrib/it-harness/package.py --check contrib/it-harness/build/Harness.ipa
```

For real install/launch, storage, CPU/memory and GLES screenshot checks:

```sh
python3 contrib/it-harness/smoke.py \
  --qemu build-native14/qemu-build/qemu-system-arm \
  --base-nand /path/to/nand-ultimate --out /tmp/harness-new-run
```

This reuses `tests/ipod/regress.py`, stages the freshly built GLES bridge only
in a disposable overlay, and requests native guest shutdown after exercising
the app. It needs the same USB/SSH tools as the existing regression harness.
Use a new output directory. Screenshots and guest results are retained there.

Verified on 2026-09-05 against `nand-ultimate`: installation, foreground launch,
preferences, 1 MiB storage integrity, CPU/memory, GLES pixel readback and the
composited screenshot passed; native guest shutdown completed. Evidence:
`/tmp/it-harness-smoke-20260905-4/boot1`. Media formats were inspected with
FFprobe; audio/video playback, networking, tilt and keyboard are implemented
but have not been interactively validated in this run.

GLES 2/Metal, camera, GPS, Bluetooth peers and microphone are outside this
device harness's current coverage. Frame rate is observed presentation-call
throughput, not a host display synchronization guarantee.
The demo initializes client vertices in resident stack memory. The current
bridge cannot fault untouched file-backed guest constant pages in; initial
testing with static vertices exposed failed host reads and a clear-only frame.

## Explicit iOS 2 fixture

The default remains the full iOS 3.1 Harness. To rebuild the legacy fixture
against a locally supplied **actual iPhoneOS 2.0 SDK**, use:

```sh
ARMV6_SDK=/path/to/iPhoneOS2.0.sdk bash contrib/it-harness/build.sh --flavor ios2
ARMV6_SDK=/path/to/iPhoneOS2.0.sdk bash contrib/it-gles/build.sh --flavor ios2
```

`--out DIR` isolates either legacy output further; default legacy outputs are
`contrib/it-harness/build-ios2` and `contrib/it-gles/build-ios2`. SDK metadata must
identify version 2.0. The compiler targets armv6/iOS 2.0, reserves r9, and uses
the existing legacy crt1/relocation conversion. Modern ld still needs its
temporary iOS 9.0 bootstrap target; audited `mkold.py` removes the incompatible
commands. A temporary copy of the SDK link stub advertises ARMv7 to ld and is
removed after the build; the SDK is neither modified nor included in the IPA.

The legacy Harness uses SDK 2.0 AudioQueue PCM output on UIKit's current run
loop. It checks buffer/enqueue/start/control/cleanup failures and retains queue
ownership if disposal fails. Compressed audio, MPMediaQuery and UIPasteboard
controls explicitly report unsupported legacy fixture coverage. The full
Harness retains its AVAudioPlayer and media features. Other manual features
still require qualification on the target guest.

`binary-audit.json` records the actual binary and supplied SDK-stub hashes,
static imports, legacy load commands and signature command. These audits and
MinimumOSVersion 2.0 prove input eligibility, not dynamic API compatibility or
successful rendering/audio. Qualify installation/listing/foreground, strict GL
pixels, and measured stereo PCM output on the target guest before adopting
artifacts as maintained defaults. No private prototype is selected automatically.

Host failure and actual-binary audit gates:

```sh
xcrun clang -Wall -Wextra -Werror -Wno-unused-parameter -Wno-unused-function \
  contrib/it-harness/test-pcm.c -o /tmp/test-pcm
/tmp/test-pcm
python3 contrib/it-harness/test-audit.py --sdk /path/to/iPhoneOS2.0.sdk \
  --binary contrib/it-harness/build-ios2/Payload/Harness.app/Harness
```

For prepared devices declaring iOS 2.x, the regression harness selects the
`build-ios2` fixtures automatically. Build the selected flavor first; a missing
legacy fixture fails preflight rather than falling back to the full Harness.
Other versions retain the full defaults. Declared deployment eligibility does
not establish runtime support: the maintained legacy Harness is currently
qualified on N72/2.1.1. Each run records requested input identities separately
in `fixture-inputs.json`.

Use explicit selections for artifacts built outside the default directories,
or to request the GLTest app instead of the default SpringBoard graphics leg:

```sh
python3 tests/ipod/regress.py --device /path/to/prepared-device \
  --ipa contrib/it-harness/build-ios2/Harness.ipa \
  --harness-ipa contrib/it-harness/build-ios2/Harness.ipa \
  --gles-app contrib/it-gles/build-ios2/GLTest.app \
  --gles-slotmap contrib/it-gles/slotmap.txt
```
