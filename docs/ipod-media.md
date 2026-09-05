# Media investigation, 7E18 (2026-09-04)

Spore's intro is MPEG-4 Part 2 video, 480×320, with AAC audio. Its game music
includes PCM16 stereo WAV files at 22,050 Hz. Implementing AAC alone therefore
does not establish that game audio works.

An isolated run installed `com.ea.spore.naj.efigsj`, launched it through
SpringBoard, dismissed the movie with Done, and rendered its menu. Both the
ordinary AMC model and the experimental `IT_AMC_STATE=1` path produced no PCM.
The latter is still diagnostic, not a production fix. Native shutdown after
entering the game completed with a guest-origin power-off event.

Later runs with the opt-in MPVD decoder changed this result: game music reaches
I2S with both the default AMC model and `IT_AMC_STATE=1`. Captures match the
game's `in.wav` after resampling (correlations 0.997 and 0.999 respectively).
The movie's AAC track remained silent in those runs. Do not conflate the blocked
movie launch with a broken PCM transport or call the AMC handshake a game-audio fix.

`IT_AMC_DECODE=1` (also accepting the earlier `IT_AMC_AAC=1`) enables the
experimental AAC-LC/HE-AAC, MP3, and ALAC DMA decoder when built with
libavcodec/libavutil. In `/tmp/it-blitz-spore-50955`, all 259 AAC frames were
decoded from 12 guest DMA submissions. The I2S capture matches all 264,821
reference stereo frames with maximum S16 error 1 (rounding). This is real guest
playback, not a host movie-player overlay. Game music follows the intro.
The prototype reads the sample-frequency index from the guest's AAC parameter
block at `0x2202ff00 + 6`, and detects mono/stereo through libavcodec's AAC
element handling. It stays gated: remaining DE programs and migration of an
active decoder are not complete.

Subsequent repeated-launch checks also pass at 22,050 Hz stereo and 44,100 Hz
mono: all reference samples reach I2S with maximum S16 error 1. Mono output
publishes one 1024-sample access unit at a time, including the last odd frame.
The 22,050 Hz clock trace exposed a separate transport bug: the guest changes
CS42L58 register 05 from `cb` to `d3`, but I2S used a fixed 44,100 Hz clock.
The codec now drives I2S through QEMU's clock connection. Both DMA pacing and
the audio voice follow it; the existing QEMU mixer converts to the host rate.
The six-second low-rate clip now lasts six seconds in the WAV backend (reference
correlation 0.99945). No extra resampling stage was added to AMC.

The nine supported codec clock encodings come from the running 7E18 driver's
rate selector at `c05dd698`, rather than a similar but different Cirrus part's
datasheet. Unknown clock modes remain unsupported.

MP3 and HE-AAC playback also match all source samples (maximum S16 error 1),
and ALAC matches exactly. Decoder initialization identifies the loaded 7E18
DE program from its descriptor, before stream parameters are populated:

| Program | Output buffers | Buffer capacity | Parameters |
| --- | --- | --- | --- |
| MP3 | 2 | 4608 bytes | Frame headers supply rate/channels |
| AAC-LC | 2 | 4096 bytes | Frequency index at parameter +6 |
| HE-AAC | 2 | 8192 bytes | Core rate split across +4/+6; SBR doubles it |
| ALAC | 1 | 16384 bytes | Depth and Rice coding values at +4…+10 |

ALAC's two client buffers alias one hardware region (`c0608a34`). Giving it
two physical regions overwrote parameters and caused repeated decoder resets;
the single-buffer ownership path fixes this. Decoded frame lengths are queued
with PCM so partial final ALAC frames are reported without padding or loss.
ALAC's rate is absent from the DE parameters because lossless decompression
does not use it. The library receives a positive placeholder in metadata only;
the independent I2S clock controls playback, as with the other codecs.

I2S retains fractional bytes between timer ticks instead of truncating them
on each tick. A runnable check verifies ten seconds of jittered ticks at all
nine supported rates drain exactly ten seconds of samples.

Real CoreAudio playback in `/tmp/it-blitz-spore-78032` covers sequential AAC,
HE-AAC, mono 22,050 Hz MP3, 24-bit ALAC, and mono AAC. There are no callback
underruns inside the five intro clips. The 44,100 Hz host captures match every
reference sample (maximum S16 error 1; ALAC exact). Low-rate MP3 reaches I2S
with maximum error 1 and CoreAudio at the correct duration; host resampling
differs from FFmpeg's reference filter (correlation 0.999272).

HE-AAC v2 needs a compatibility qualification: 7E18 negotiates its mono core.
The decoder therefore enables SBR and disables parametric stereo, rather than
feeding stereo samples into a mono guest stream. `/tmp/it-blitz-spore-80111`
matches the SBR-only reference through all 264,822 frames, maximum error 1.
This is not full stereo PS support. Subsequent offline reference-device checks
below confirm that 7E18's HE hardware program also exposes mono for this file.

Decoder errors drain valid queued frames, then
publish one silent error buffer with status bit 0 and error 100 at `0x2202ff28`.
Input DMA ownership is returned after that output is published.
The code is taken from the guest driver's own no-output path. Buffer ownership
and interrupt acknowledgement still apply. A new job clears both the error
and old decoder state. ASan/UBSan checks cover these transitions, including
failure before a codec exists. `/tmp/it-blitz-spore-80294` plays a copied movie
with a deliberately corrupted AAC packet, then replays the healthy movie in
the same guest. All 264,821 healthy replay frames match with maximum error 1;
native shutdown succeeds afterward.

The native AudioCodec API requires packet boundaries and discards subsequent
raw frames inside a declared packet. AMC's first DMA contains 39 concatenated
raw frames without ADTS framing. libavcodec consumes all 39 directly. The two
output regions are alternating interleaved PCM buffers; publishing both under
one completion, or waiting for both to be released, stalls the guest. Each
buffer has independent ownership and requires an acknowledged completion.

Runnable checks (the PCM reference is decoded from the actual source movie):

```sh
python3 tests/ipod/test_amc_aac.py
python3 tests/ipod/test_codec_clock.py
ffmpeg -i Intro.mov -vn -ar 44100 -ac 2 -f s16le reference.pcm
python3 tests/ipod/test_pcm_capture.py capture.pcm reference.pcm
```

The AAC check uses ASan/UBSan against the actual C helpers, including a cyclic
DMA chain, DRAM boundary rejection, bounded buffering of 1000 frames, independent
output ownership, and recovery on a new job. The capture check requires NumPy.
For mono reference conversion use `-af 'pan=stereo|c0=c0|c1=c0'` to duplicate
samples at unity gain, as the guest does. Crop repeated clips to their individual
capture intervals before alignment. Raw I2S captures use the guest clock and
can change rate within one file; the WAV backend uses a fixed host rate.

## Video contract

### H.264

The original H.264 aperture at `0x38f00000` was plain RAM. The new opt-in
`IT_H264_DECODE=1` device implements the 7E18 M2H264 bit reader and progressive
CAVLC/CABAC I/P reconstruction. Device-tree IRQ is 35. Migration is explicitly
blocked for this experimental device. Default configurations retain RAM.

`0x1200` holds the input address in 1 KiB units; `0x180c/0x1810` bound one
NAL. Command `0x1600=0x801` consumes its header and exposes ready/type/ref-idc
at `0x1628`. Reads at `0x1400 + 4*n` consume n bits (zero means 32).
Exp-Golomb commands `0x1078=1/3` peek unsigned/signed codes; the guest then
consumes the reported count separately. Treating that accelerator as a
consuming operation misparses every subsequent slice field.

The guest submits reconstruction at `0x1000=1`, after parsing the slice header.
The model synthesizes equivalent SPS/PPS/slice headers from hardware fields,
preserving the compressed macroblock payload. Output Y/C address-register
indices come from `0x106c`; L0 reference indices start at `0x100`. Physical
address entries start at `0x1200`, in 1 KiB units. Thus fixed `0x120c/0x125c`
output addresses work only for the first buffer, not subsequent P pictures.

Native decoders hide their DPB, so the bridge seeds it with lossless I_PCM
pictures read from the guest's selected reference planes. P reconstruction
therefore follows actual DMA contents rather than a second ownership cache.
This covers up to 16 active reference entries. Repeated Y/UV addresses share
one synthetic picture identity; reference-list modifications reproduce the
guest list, including duplicates used by weighted prediction. CABAC
initialization comes from `0x10cc`; weighted P prediction uses the
denominators at `0x1054/0x1058` and packed signed weights/offsets at
`0x400/0x480`. B-direct motion vectors and field pictures remain unsupported.

`0x1038/0x103c` specify the slice's starting macroblock row/column. If the
native decoder cannot complete a picture, the existing libavcodec dependency
provides incremental slice decoding (`AV_CODEC_FLAG2_CHUNKS`). The package
build now enables its H.264 decoder as well as AAC/MP3/ALAC. Reference planes
are still seeded from guest DMA at the start of each picture. Slice jobs may
split rows and vary QP/CABAC/weights; the current implementation requires the
same destination throughout a picture and commits DMA only when the picture
is complete. Later slices may reorder, repeat, subset or introduce references;
new references trigger bounded replay of the unfinished picture. Intermediate
partial-pixel visibility remains unimplemented.

The packaged FFmpeg applies `contrib/ffmpeg/h264-chunk-er.patch`: end-of-picture
error concealment must wait for the final chunk. Running it at every packet
boundary treats later slices as missing and overwrites valid earlier slices.
The bridge now enables error-resilience bookkeeping and rejects every frame
error flag, including DECODE_SLICES. Earlier builds disabled bookkeeping and
ignored that flag; the missing-macroblock check below exposed the resulting
false success. These checks do not establish universal malformed-stream
handling or exact hardware error status.
Success reports `0x1074=1`, errors `=2`; `0x10c0=0` clears IRQ. Software-reset
bits at `0x1004` self-clear. No successful completion is reported on failure.

`tests/ipod/test_h264_reader.py` checks the MMIO reader under ASan/UBSan.
`tests/ipod/test_h264_native.py` compares the actual native bridge and every
DMA pixel against original native decoding. It derives short-term reference
lists and weighted duplicates from ffmpeg-parsed original headers, and covers
Baseline/Main, multiple references, CABAC initialization 2 and weighted P
prediction. `--software` also checks multi-slice decoding, deferred DMA,
rejection of a changed destination/orphan continuation, and recovery. All 180 frames pass for each fixture. The actual guest
multi-reference run `/tmp/it-blitz-spore-81422` also accepts all 180 completions;
all 41,472,000 captured NV12 bytes exactly match an independent FFmpeg decode.
The CABAC and weighted P guest runs (`/tmp/it-blitz-spore-81599` and
`/tmp/it-blitz-spore-81895`) also match every captured NV12 byte.
These early runs established reconstruction; later presentation checks below
also validate actual on-screen playback.

`IT_VIDEO_DUMP=/path/to/capture.nv12` appends native output planes before guest
composition (only real output frames, excluding reference priming). Geometry
comes from the workload. The shared `ipod_video.c` uses VideoToolbox for both
MPVD and H.264. Synthetic H.264 headers have no VUI and request nominal limited
range so coded samples remain unchanged; original full-range reference movies
must request full-range output when comparing raw samples.

### MPEG-4 Part 2

The kernel log names `AppleMPVDDriver`, not the separate H.264 aperture:
"Was waiting for interrupt, but something unexpected happened." The default
MPVD model is register backing only. `IT_MPVD_DECODE=1` enables the native
MPEG-4 I/P prototype, with real DMA output and IRQ 45 completion.

Enable QEMU's `-trace 'enable=ipod_touch_mpvd_*'` for register traffic.
In the captured first job:

| Register offset | Observed value / role |
| --- | --- |
| `0x60018` | Input initially `0x0f5c2000`, then `0x0f5c2004` |
| `0x6001c` | Input end `0x0f5c2786`; includes more than the encoded packet |
| `0x6006c` | `0x001e0014`, consistent with 30×20 macroblocks |
| `0x6003c`, `0x60044` | `0x0f644400`, `0x0f669c00`; possible output planes, not yet proven |
| `0x41200…0x412fc`, `0x41300…0x413fc` | Two 64-entry quantization tables |

Live RAM at `0x0f5c2000` begins `000001b6`, the VOP start code. Its bytes match
the first video packet in Intro.mov at file offset 22490 (1667 bytes). The
programmed input end extends beyond that packet, so it must not be treated as
an exact encoded length without checking the driver's padding contract.
Buffers are reused after Done; a later RAM dump contained menu pixels at those
same addresses. Capture while the decode job is pending.

macOS native decoding was tested separately: AVAssetReader decoded all 180
frames with non-black content, and VideoToolbox decoded all 180 compressed
packets using the track's MPEG-4 format description. Reader metadata markers
can have no format description or data buffer; skip them. The native decoder
requires its media service, so sandbox denial must not be misdiagnosed as an
unsupported codec. No FFmpeg dependency is needed for this demonstrated codec.

The generated configuration now comes from guest dimensions and time-bit width.
`tests/ipod/test_mpvd_native.py Intro.mov 480 320 10` verifies that all 180
decoded Y/UV frames exactly match decoding with the original movie format.
The guest accepts all 180 completions without its previous driver timeout.
Presentation is still wrong (white movie rectangle), so decoding remains gated.
Still required: verify the scaler/compositor consumes the produced planes and
extend the supported hardware configuration beyond the tested I/P subset.
Do not hardcode Spore's VOL header or locate a host movie file as a substitute
for emulating the guest job.

## Evidence artifacts

- `/tmp/it-blitz-spore-40817`: game-menu screenshot, empty PCM capture, kernel
  log and RAM, clean shutdown with the experimental AMC handshake.
- `/tmp/it-blitz-spore-41908`: MPVD trace plus RAM captured with the first packet
  still present. Initial launch was foreground but dark; later warm reset and
  native shutdown both passed. This is not a playback PASS.
- `/tmp/it-mp4-native.m` and `/tmp/it-mp4-vt.m`: native decoder probes against the
  extracted movie; `/tmp/it-mp4-vt.log` records the packet-level result.
- `/tmp/it-blitz-spore-52833`: repeat playback at 44,100 then 22,050 Hz, both
  sample comparisons pass; codec clock returns to 44,100 Hz for game music.
- `/tmp/it-blitz-spore-53050`: repeat stereo then mono playback, both sample
  comparisons pass; native guest shutdown completes.
- `/tmp/it-blitz-spore-55365`: AAC then MP3; all 264,821 MP3 reference frames
  match with maximum S16 error 1.
- `/tmp/it-blitz-spore-72639`: AAC then 24-bit ALAC converted to device S16;
  all 264,821 reference frames match exactly, with two stream initializations
  and no decoder errors.
- `/tmp/it-blitz-spore-75068`: AAC then HE-AAC; all 264,822 reference frames
  match with maximum S16 error 1 and no decoder errors.

## Native compositor and scaler (September 5)

The forced software compositor cannot sample NV12: 7E18 QuartzCore's format
selector at `31b92ff8` rejects `420v`/`420f`, and the fallback sampler at
`31bc6c48` paints white. Enabling `CA_ENABLE_OGL` and `LK_ENABLE_OGL` in a copied
SpringBoard launch plist reaches the native compositor, but exposed separate
bridge failures:

- `GLESBindCoreSurface` takes `(gc, target, surface)` (stock engine `d918`).
- `GLESCreateSharegroup` stores through argument 0 and returns 1 (`a0d4`).
- The compositor queries `GL_MAX_RECTANGLE_TEXTURE_SIZE_ARB`; rejecting it
  leaves its tiling limit zero and causes recursive subdivision/stack overflow.
- Context state must be separate, with GL objects shared only within a
  sharegroup. Software VBO references in sibling contexts must survive deletion.
- Imported IOSurface textures must observe later guest DMA writes. Sampled
  aliases are refreshed before drawing; the current render target is excluded.

With those ABI fixes, the movie previously stopped after seven decoded frames.
`/tmp/it-blitz-spore-83404/device/qemu.log` proves the compositor submitted a
scaler job and waited on the unimplemented peripheral. The actual job converts
NV12 into RGB565 (not packed YUV): input format `0`, output format `4`,
480×320, Y/UV pitches 480, output pitch 480 pixels. The stock driver's format
switches at `c073118c` / `c073129c` establish 0=NV12, 4=L565, 6=BGRA.
The scaler DeviceTree node has VIC interrupt `0x25`.

`IT_SCALER_DECODE=1` replaces that stub with register backing, reset, completion
mask/status/W1C, and the observed unscaled NV12-to-RGB path. It applies the
programmed signed 12-bit matrix coefficients at `0x220..0x240` with 9 fractional
bits. Source range selection uses control bit 9. Source/destination bounds are
validated before DMA; source planes are snapshotted to tolerate aliasing.
The current model does not implement resizing, crop offsets, rotation, other
input formats, or hardware-exact chroma filtering/rounding. Unsupported jobs
report an error and complete without modifying their output; that completion
prevents deadlock but is not a claim that the transfer was rendered correctly.

`tests/ipod/test_scaler.py` checks conversion, padding, invalid DMA, interrupt
mask/ack and reset under ASan/UBSan. Native graphics checks are
`test_gles_context.py`, `test_gles_surface.py`, and `test-gles-boundaries.py`.
The first scaler-enabled guest (`83691`) decoded all 180 movie frames but
sampled stale imported textures, producing black video. The subsequent texture
refresh change remains under guest validation. Its shutdown check failed while
staging the helper (private USB connection dropped); do not count it as a clean
halt test. All tests used disposable NAND overlays, with the user instance
untouched.

### Hardware video scanout (September 5)

The black movie after a successful decode was not a failed texture draw. Native
GL readback during the transition contains the movie's colors. Once fullscreen,
7E18 switches to the CLCD video plane and leaves transparent controls in a
separate RGB plane. The old LCD model ignored both planes.

`IT_LCD_PLANES=1` enables the initial LCD compositor. Firmware provenance is
AppleM2CLCD at `c05e3640..c05e3efc` in the 7E18 kernel:

- `4[3]` enables NV12 video, `4[8]` selects limited-range luma; `4[4:5]`
  enable the two RGB windows.
- `11c/120` are Y/UV physical addresses; `2e0/2e4 >> 17` are byte strides.
- `130/134` are source origin/size and `138/13c` destination origin/size,
  with X/width in the high halfword.
- `70..90` contain a row-major color matrix with ten fractional bits and
  sign-magnitude bit 12. These are not the standalone scaler's signed Q9 values.
- `118` is the video transform; observed mode 3 matches the clockwise CA
  transition quad. RGB controls use premultiplied BGRA in window 2.

The register bank is latched at vblank and included in LCD VMState version 2.
The sanitizer check is `python3 tests/ipod/test_lcd_planes.py`. It covers NV12
conversion, rotated scanout, premultiplied controls, stride padding and invalid
DMA. A private guest run in `/tmp/it-blitz-spore-85097` displays moving decoded
color-pattern video throughout all six seconds, with the status bar visible.
That first run exposed a reversed video rotation, corrected to match the
observed CA transition geometry. `/tmp/it-blitz-spore-85318` then displays
the native Main-profile CABAC H.264 intro in the correct orientation, including
the EA animation, Spore title and controls/status bar. The original MPEG-4
intro is visible in `/tmp/it-blitz-spore-85223`; that run predates the rotation
correction. Controls are still washed out, and post-playback SSH/shutdown
remains under investigation. Audio is sample-exact for the opening seconds
but diverges later in these full-RAM-capture runs; do not call combined media
playback fully validated yet.

This is not complete CLCD emulation: scaled video requires the programmed
polyphase filters; additional transforms, RGB formats, source cropping and
arbitrary layer priority are not implemented. Unsupported configurations fall
back to the existing scanout and report once. Keep this opt-in until the original
intro, H.264 playback, controls and shutdown checks are complete. Sparse scaler
jobs are expected when the display scans decoded NV12 directly; they were not
proof of a stalled decoder or missing swap notification.

The combined-media audio discrepancy above was caused by the intrusive
128-MiB `pmemsave` during playback. Removing that capture gives all 264,821
intro stereo frames at I2S with maximum S16 error 1 in
`/tmp/it-blitz-spore-85562`. `/tmp/it-blitz-spore-85702` additionally captures
the native CoreAudio IOProc: all 264,821 frames match at offset 2,439,312,
maximum error 1, with **zero starved callbacks during content**. Idle callbacks
are silent and should not be counted as audible underruns. USB still stops
responding without the RAM capture, so that is a separate unresolved failure.

Native compositor image uploads had a separate demand-paging failure:
`cpu_memory_rw_debug()` cannot fault missing guest mappings in. The metadata
was valid, but imports of cached icons, labels and button backgrounds returned
failure before uploading pixels. `GLESBindCoreSurface` now performs bounded
volatile reads of each source page while the IOSurface is locked, allowing the
guest VM to resolve those mappings normally. No host decoder or image substitute
is involved. `/tmp/it-blitz-spore-85914` restores the blue buttons, gradients,
rounded player controls and clock text while the original MPEG-4 intro and
all 264,821 audio frames continue to pass (maximum S16 error 1). The native
surface check includes the page-touch loop, overflow/stride rejection and an
actual fixed-function textured draw, not just texture storage/readback.

### Post-playback core-voltage deadlock

The remaining USB/shutdown failure was a kernel spin in
`AppleS5L8720XSWICoreVoltageFunction` at `c05f47e0`, repeatedly reading SWI
channel 1 control register `0x1c` while bit 0 remained set. SWI at `3de00000`
was plain RAM. The 7E18 driver starts channel 0/1 through `14/1c`, after
writing voltage commands to `18/20`; starts use 1 or 3, and the driver polls
busy bit 0 before reusing the channel. The new `ipodtouch.swi` device completes
those commands synchronously, preserving configuration and command words.
It models the firmware handshake, not voltage waveform timing or host voltage.
`python3 tests/ipod/test_swi.py` checks repeated commands on both channels.

With that fix, `/tmp/it-blitz-spore-86217` plays the original MPEG-4 intro with
correct controls and orientation, then successfully answers both SSH and
`ideviceinfo`, and completes guest-origin `ithalt` shutdown. I2S and native
CoreAudio both match all 264,821 intro frames within one S16 step. The native
callback tap has no starvation inside the intro; five empty callbacks occur
only in the silent transition between the movie and subsequent game music.
These must not be classified as a dropout inside either clip.

The corresponding Main-profile CABAC H.264 run, `/tmp/it-blitz-spore-86282`,
also passes playback, USB queries and guest-origin shutdown with the normal
30-second SSH connection persistence. All 180 decoded NV12 frames match the
reference decoded from that exact input movie byte-for-byte (41,472,000 bytes).
Native CoreAudio matches all 264,821 intro frames within one S16 step.

A Main-profile B-frame fixture in `/tmp/it-blitz-spore-86372` is rejected by
the guest player with “This movie is damaged and cannot be played,” before
any H.264 hardware accesses. This does not validate B-frame decoding or
identify a hardware-model failure. B pictures remain unsupported in the model.

### Scaled playback and reset follow-up

The 240x160 H.264 fixture decoded correctly but exposed an unsupported LCD
configuration: the guest requested 320x480 rotated scanout with both Q16 ratios
set to `0x8000`. The compositor now uses the programmed filters. Kernel
`c05e20d0..c05e21e8` uploads nine half-phases to `140` (8-tap luma horizontal),
`1d0` (4-tap luma vertical), `220/270` (4-tap chroma horizontal/vertical).
Each word holds two signed 12-bit Q7 coefficients, high halfword first;
remaining phases reverse the stored half-phase taps. The implementation uses
separable filtering, edge extension and intermediate byte rounding. Exact
hardware edge/rounding behavior still needs a physical capture; this is not
a claim of bit-exact CLCD output. The test covers packed negative coefficients,
mirrored phases, edges and rotated scaling with an independent linear kernel.

`/tmp/it-blitz-spore-86617` displays the 2x enlarged movie, matches all decoded
pixels and all 264,821 native audio frames, and shuts down cleanly. A larger
640x432 movie also displays correctly (`86709`), but that first run has an
opening audio mismatch; do not count it as a full audio pass. Native decode
jobs measured roughly 8 ms with per-picture session creation. The H.264 model
now reuses unchanged native formats/sessions while continuing to seed each
job from current guest reference planes starting with an IDR. This reduces
measured jobs to roughly 6–7 ms without introducing a second reference owner.
Format changes, errors and machine reset discard the session. The native test
checks all pixels across reuse and deliberate mid-stream session recreation.
The larger movie repeat `/tmp/it-blitz-spore-87067` matches every decoded pixel
and the complete I2S/native audio track, and passes USB queries and shutdown.
This repeat alone does not establish the cause of the earlier timing mismatch.

Native GLES reset cleanup now releases live contexts, sharegroups, legacy
state and guest DMA aliases. Handles remain monotonic. Sanitizer tests include
a context that outlives its deleted guest sharegroup, stale handles and
repeated reset. `/tmp/it-blitz-spore-86932` plays H.264, warm-resets in-process,
then plays again: both 180-frame videos are byte-identical to the reference,
both complete native audio clips match within one S16 step, and USB plus
guest-origin shutdown pass. This guest run predates native H.264 session reuse;
`/tmp/it-blitz-spore-87163` repeats the reset check with the reused session and
640x432 scaled video: all 360 decoded frames match, both complete native audio
clips match within one S16 step, and clean shutdown passes.

### Packaged LightTouch integration

An isolated Release build at `/tmp/it-media-app-build/Build/Products/Release/LightTouchMac.app`
embeds the updated emulator, guest engine and minimal FFmpeg closure. Packaging
found that FFmpeg needed its own `@loader_path` rpath to locate sibling libraries;
the dependency build now supplies it. The complete bundled Mach-O closure passes
macOS 14 deployment/relocation checks and ad-hoc signature verification.

The test uses bundle identity `gold.samhenri.LightTouchMac.MediaTest`,
`CFFIXED_USER_HOME=/tmp/it-media-app-home`, a copied overlay, and explicit media
environment flags. Foundation's home and Application Support resolution were
verified before launching. The user's running app and state were not modified.
Spore's original movie is visible in the app window; its 180 native-decoded
frames match the earlier standalone native run. Both I2S and CoreAudio match
all 264,821 intro frames within one S16 step, with zero content-starved callbacks.
Do not substitute ffmpeg pixel equality for the MPEG-4 check: native MPEG-4
reconstruction differs slightly from ffmpeg, unlike the tested H.264 fixtures.

Normal app termination also exposed a lost SSH stdout acknowledgement: sshd
can close before the halt marker is delivered. LightTouch now waits for the
authoritative PMU event even when that acknowledgement is absent. The repeat
logs `guest confirmed power-off — volume unmounted` and exits zero. Artifacts
are under `/tmp/it-media-app-home`; `/tmp/it-test-media-app.py` performs playback,
private USB queries, window captures and normal termination.

This is still an explicitly enabled test build with an already-upgraded copied
guest. Existing guest component upgrades and default media activation are not
yet shipped. The first app test waited longer than the diagnostic tap's fixed
180-second capacity before playback, so its silent native tap is not an audio
failure; the immediate repeat above captures the complete track.

### Aspect-fill cropping

The 4:3 movie's zoom control reproduces a black fullscreen picture with source
origin `130=13`, source size `134=320x213`, destination `13c=320x480`, transform
3 and ratios `2c0=0xaa66`, `2c4=0xaaaa`. The source rectangle can have odd origin
and extent even though the underlying decoded NV12 picture is even-sized.
The compositor now bounds the full source footprint, reads the cropped luma
and containing UV rectangle, and carries odd luma coordinates into half-sample
chroma filter phases. The sanitizer test checks odd X/Y crops and DMA bounds.
`/tmp/it-blitz-spore-89319` shows the enlarged moving movie after zoom, with
controls and status bar, and passes USB queries and clean shutdown. Filter
edge extension remains relative to the supplied crop; physical edge/rounding
comparison is still outstanding. All 795,253 stereo audio frames and all
decoded NV12 bytes in the 18-second zoom fixture pass their references.

### Existing-device upgrade in LightTouch

With the media flags enabled, LightTouch now waits for USB readiness before
accepting input, compares the bundled graphics engine with the guest's copy,
and enables only `CA_ENABLE_OGL`/`LK_ENABLE_OGL` in the existing SpringBoard job.
Foundation preserves XML/binary plist format and unrelated keys; malformed
jobs fail before either guest file is changed. Updated files are staged beside
their destinations and renamed atomically. SpringBoard is reloaded only when
components changed. Shutdown/restart cancel and await preparation first.

The shared SSH transport now carries command/password/path arguments without
shell interpolation, collects binary output, and uses a bounded task group with
process-group teardown. `LightTouchMac/tests/check-media-components.py` checks
plist preservation/idempotence/rejection and the actual shell's binary input,
quoting, and cancellation cleanup using a fake transport.

The separate-identity packaged app upgraded a copy of the older `41908` overlay
in `/tmp/it-media-upgrade-home`. The first run plays the original MPEG-4 movie,
matches all 180 native video frames and all 264,821 I2S/CoreAudio audio frames,
and confirms guest power-off on normal quit. Its artifacts are under `first/`.
The next launch reports components already current, plays the 640x432 H.264
fixture with all NV12 bytes exact and all native audio frames within one S16
step, and again confirms clean shutdown. Media remains opt-in while active
native decoder/graphics snapshot state is unfinished.


### Long game audio and an older corrupt fixture

A 90-second packaged-app run exposed corruption in the installed `in.wav`
asset, not a difference between I2S and native output. Reading the asset from
a fresh, unplayed copy of the older `41908` overlay gives the same SHA-256
`ec68ee93ed5fa5492b63a3138c324ecb07567e37c2da50af390dd16d0139a22d`;
the original IPA member is
`f0b1d9dcb8a94bccb30373108f2223c249a0d3e08995f995ace760867441838e`.
Differing page ranges are `0x207000..0x3e7000` and `0x607000..0x627000`.
This corruption predates the media changes; its original cause is not yet
established. Do not count the old installed music as a valid source fixture.

Replacing only the copied test device's WAV with the IPA member gives exact
readback both before and after 90 seconds of playback. The whole 73.186-second
track keeps one alignment against the original resampled PCM, with left/right
correlations 0.999903/0.999896 (the guest's mixer/resampler changes gain/filtering).
All 3,227,520 stereo I2S frames reach native CoreAudio with zero sample difference.
The app remains responsive and confirms guest power-off on normal quit.
Artifacts are `/tmp/it-media-upgrade-home`, with the earlier corrupt-track
capture retained in `long/`, and the unplayed baseline read in
`/tmp/it-media-wave-baseline-home`. The user's app and installed files were not
modified.

Quitting during the actual startup component check is also validated in
`/tmp/it-media-cancel-home`: the update is cancelled, guest power-off is confirmed,
and the isolated app exits zero in 2.65 seconds without applying the update.


### Incremental H.264 and duplicate reference identity

A four-slice Baseline fixture reaches 720 guest jobs but native VideoToolbox
rejects each incomplete picture (`91828`). The incremental fallback displays
it in `97497`, with all 41,472,000 decoded bytes exact, all 264,821 I2S/native
audio frames within one S16 step, responsive USB, and clean shutdown. The
sanitizer/reference test also passes weighted CABAC slices split every 70
macroblocks, including starts inside a row.

A four-reference, weighted CABAC fixture uses up to six active L0 entries
because some entries repeat the same reference picture. Treating every entry
as a distinct seeded picture changes deblocking identity: the first mismatch
in `97673` is two one-step pixels in frame 2, then errors accumulate. The shared
native/software path now seeds unique Y/UV pairs and rebuilds the actual L0
list with explicit short-term reference modifications, retaining duplicates.
The repeat `97788` matches every decoded byte and the complete native audio
track, and passes USB/shutdown. The expanded reusable test independently
reconstructs those reference lists from the original bitstream and passes all
180 multi-slice frames under ASan/UBSan.

The single-slice native path also passes the weighted four-reference fixture.
Warm-reset run `97908` produces two byte-exact 180-frame picture sequences,
but exposes a separate audio channel-alignment defect: the second clip matches
only after shifting the native capture by one S16 sample. The I2S ring is cleared
on reset while its lifetime byte counter survives; using that counter to pad
the next FIFO reset inserts an incorrect half-frame. Alignment now follows the
retained ring position. `tests/ipod/test_i2s_alignment.py` covers stale lifetime
counts, true partial frames, and ring wrap. Repeat `98666` passes both complete
264,821-frame stereo clips at native capture offsets 2,443,814 and 5,016,569,
with maximum error one S16 step. All 360 video frames remain byte-exact, USB
responds after reset, and guest shutdown is confirmed.

### Volume control and bulk-file persistence follow-up

The rebuilt isolated app (`99459`) displays the weighted multi-slice movie with
all decoded pixels exact and all 264,821 native audio frames within one S16 step,
then confirms guest power-off and exits zero. Its cold-read check finds that the
previously repaired music file did not persist completely. Offline HFS catalog
and extent reads reproduce the same bytes, ruling out SSH or playback decoding.

Fresh write/reset run `99777` identifies the cause: FMSS stops after 512 script
entries, dropping file pages 512–991 and 1543–1574 while appearing successful.
Immediate readback succeeds from the guest's cache; reset exposes the loss.
The write path now accepts larger terminated scripts, bounds every DMA read to
actual RAM, and stops the VM on malformed commands. The device-free regression
checks all 1,024 persisted pages plus DMA bounds and existing host-I/O failures.
Fixed run `100` reads back all 6,455,084 bytes exactly after warm reset; a separate
cold boot in `201` does too. The reconstructed full 7 GiB volume passes
`fsck_hfs -n` with exit zero (`/tmp/it-bulk-fixed-fsck-confirm.log`).

Volume run `99365` changes the visible HUD but every native sample remains at
unity. I2C run `99609` records amplifier commands `b3, ab, a3, 9b, 8b, 83, 73,
5b` as volume falls. The LM48821 stub wrongly consumed each command as a register
address. [TI SNAS354A, tables 2 and 3](https://www.ti.com/lit/ds/symlink/lm48821.pdf)
specifies a complete control word per byte: gain in bits 7:3, mute in bit 2,
and left/right enables in bits 1/0. The model now handles those controls and
their gain table, with host PCM scaling, clipping and a bounded
`IT_I2S_GAIN_DB` calibration override. The raw I2S tap remains before amplification;
native unity comparisons require accounting for output gain. Amplifier checks
are in `tests/ipod/test_audio_volume.py`. Native run `201` matches independently
scaled I2S samples exactly at 15 stable windows covering +6, +4, +2, 0, −4, −6,
−10 and −16 dB and the return to +2 dB. Its entire final 5.5 seconds also matches,
so the end of the clip is not cut off by amplifier shutdown. USB and clean guest
shutdown pass. Codec analog routing/gain, amplifier transient behavior and
absolute output calibration remain separate fidelity work.

Discrete volume presses in `1247` reach command `04` during the movie, then
restore gain through `5b` up to `b3`. Five seconds of non-silent source produce
exactly zero native samples while muted; output returns afterward. The standard
`restart` and `persist` regressions now write markers larger than 4 MiB so their
normal coverage crosses the old bulk-write limit.

The current isolated package also passes the 90-second game run (`1355`): all
3,227,520 captured game I2S frames reach CoreAudio with exactly the expected
+6 dB amplifier scaling. The original WAV comparison shows a six-frame startup
timing adjustment, then no further shift at 10, 30 and 60 seconds; the guest
mixer/resampler contribution is not yet isolated. The repaired private WAV is
exact before and after playback. Separate packaged cold boot `1632` reads back
the same bytes, displays all 180 weighted multi-slice H.264 frames exactly, and
delivers the entire intro audio with exact amplifier scaling. Both runs confirm
guest power-off and exit zero. Existing user files damaged by the former bulk
write bug have not been altered or automatically repaired.

### Physical audio reference and end-of-input ordering

The connected reference iPod2,1 runs the same 3.1.3 / 7E18 firmware. Its installed
decoder list matches the emulator's: AAC, HE-AAC, MP3 and ALAC have hardware
decoders; there is no separate `aacp` decoder. Listing formats alone does not
prove channel behavior, so `contrib/it-audio/offline.c` now provides a bounded
AudioQueue hardware-only probe that renders to a file without speaker output.
Temporary reference-device files are removed afterward; no apps or firmware
are changed.

AudioFile describes the implicit-SBR fixture as AAC-LC, 22,050 Hz, mono.
ExtAudioFile instead exposes HE-AAC, 44,100 Hz, mono. Both selections are valid:
the LC hardware program ignores SBR/PS and the HE program applies SBR without
PS. The emulator previously let libavcodec implicitly promote the LC program,
then rejected its changed rate/buffer layout, producing silence (`2617`,
`2673`). The synthesized LC configuration now explicitly disables SBR.
Library extension warnings are trace-only; returned decode errors still take
the guest-visible error path.

That correction exposes premature input completion. The emulator acknowledged
the final compressed input while 32 decoded frames remained in its software
queue. AudioQueue treated the acknowledgement as end-of-stream, truncating
the offline result at sample 108,544 (`2770`, `2839`, `2928`, `2977`). Priming
or enlarging the probe's buffer did not fix that ordering bug. AMC now publishes
the queued PCM before returning input ownership; error completions likewise
follow valid queued output and the final error buffer. The actual timer path
is covered in `tests/ipod/test_amc_aac.py`.

Fixed run `3096` matches all 278,528 stereo output frames of the physical LC
reference within one S16 step. The checked-in probe repeats both selections
on the physical device and emulator in `3283`: LC maximum error 1; HE maximum
error 13, RMS error 0.518 S16 steps across both channels, and left-channel
correlation 0.99999994. Both physical outputs and both emulator outputs contain
mono in the left channel and silence in the right under this explicit stereo
offline format. Thus full parametric stereo is not an established missing
capability of this firmware's HE hardware decoder. Every emulator run here
confirms USB responsiveness and PMU shutdown.

Normal playback run `3354` covers AAC, HE-AAC, 22,050 Hz mono MP3, ALAC and
mono AAC after the completion change. Every decoded reference sample reaches
I2S (maximum error 1; ALAC exact). The four 44,100 Hz clips reach CoreAudio
with exactly the amplifier-scaled samples. A stricter low-rate MP3 comparison
against QEMU's own linear resampling finds approximately 21 ms missing from
the quiet tail at the subsequent rate change; this is separate from decoding
and remains under investigation. Error/recovery run `3524` plays healthy AAC,
a corrupted packet, then weighted multi-slice H.264 without restarting. Both
healthy audio captures are complete with exact native amplifier scaling, and
all 41,472,000 bytes of the H.264 replay match the reference. USB and clean
shutdown pass in both runs.

Rate-transition trace `3650` locates the tail loss in the I2S host ring: it
still contains 14,390 old-rate bytes when LRCLK changes from 22,050 to
44,100 Hz. The host voice itself has no mixed or pending resampler frames at
that point. Applying the new rate and amplifier setting to that backlog
changes audio that was already queued. Each retained stereo frame now carries
its FIFO-entry rate/control word in a fixed-size metadata ring. Gain changes
apply to subsequent frames, and the host voice changes rate only at the matching
queue boundary. Reopening happens on the existing device timer, outside the
audio callback's voice iteration. Reset discards the queue; new writes replace
metadata along with their samples.

`tests/ipod/test_audio_volume.py` executes the real push/drain/rate-change path
and covers partial frames, ring wrap and later control changes. Clock checks
still require immediate DMA pacing changes while host playback retains queued
formats. Native repeat `3859` passes all 264,822 resampled MP3 output frames,
including the formerly missing tail, with maximum error 0.5 S16 step against
the amplifier-scaled I2S data and QEMU's linear interpolation. The following
264,821-frame AAC clip is exact after amplification. USB and PMU shutdown pass.

Warm-reset repeat `3931` preserves both complete 264,821-frame audio tracks with
exact amplifier scaling and all 360 H.264 frames byte-for-byte. Packaged run
`4601` passes cold-file persistence, the full low-rate MP3 tail, a switch to
weighted H.264, native window captures, and normal PMU-confirmed quit. The
Release package then enables all five media paths in `setBootEnv` and performs
the existing component preparation on ordinary AppSync launches. Repeat `5330`
explicitly removes all media-enabling environment variables before starting
the app: both audio tracks and all 180 H.264 frames pass again, and the app
exits zero after guest power-off. Snapshot support remains separate work;
enabling playback does not make active decoder/graphics state migratable.

### H.264 reference identity across slices

`tests/ipod/test_h264_slices.py` reproduces a later slice changing its active
reference list. The original continuation key included the list order/count,
so a valid change failed before decoding. Continuations now match physical
Y/UV plane pairs to the picture's initially seeded DPB and explicitly reorder
that stable list. Frame numbers and deblocking reference identity stay fixed.
The sanitizer check uses independently known flat reference planes and covers
reordering, a smaller active list, duplicate entries, and rejection of a valid
but unseeded reference. Both the weighted CABAC multi-slice software fixture
and its single-slice native counterpart still pass all 180 frames pixel-for-pixel.

Per-slice DMA remains deferred until picture completion. FFmpeg's public
horizontal-band callback reports completed rows (with deblocking delay), not
the exact final macroblock of a slice ending partway through a row. It alone
does not establish hardware progress or safe publication for missing slices.
References absent from the initial slice initially remained unsupported;
the bounded replay follow-up below removes that restriction.

### H.264 incomplete-picture rejection

A synthetic P-picture omitting one row reproduced false success with error
resilience disabled: the destination received an incomplete frame. The root
fix is in FFmpeg's chunk path, not a second decoder or a guessed coverage map.
`contrib/ffmpeg/h264-chunk-er.patch` delays `ff_er_frame_end` using the same
picture-completion condition as FFmpeg's chunk output path. Slice coverage
bookkeeping stays enabled across packets. The bridge rejects all frame error
flags and commits no DMA on malformed-picture failure.

The native package build applies and includes the patch alongside its existing
FFmpeg source/build notices. Set `PKG_CONFIG_PATH` to that prefix for the
software H.264 checks; an unpatched system FFmpeg still conceals each chunk.
The sanitizer check covers a missing row, one missing macroblock, and one
overlapping macroblock, as well as reference changes. Both the CAVLC and
weighted CABAC multi-slice fixtures pass all 180 frames with strict checks.

The whole-engine `0x1004=0x7ff` initialization now discards native/software
decoder state. The driver's per-NAL `0x0c` reset preserves partial pictures.
Both cases are covered by the same sanitizer check; individual undocumented
pipeline reset bits still need separate hardware evidence.

Strict-validation guest `6165` retains all 360 reference-exact frames across
warm reset and clean shutdown. Error/recovery guest `6327` replaces the second
slice of frame 60 with a legal filler NAL, preserving the MOV layout. The first
60 pictures match, the damaged picture reports `0x1074=2` and commits no frame,
and reopening the healthy movie produces all 180 reference-exact pictures.
USB remains responsive and PMU shutdown succeeds without restarting the guest.

A later I slice can now finish a picture begun with P slices. It keeps the
picture's non-IDR NAL identity and frame number instead of being rewritten as
a new IDR. The synthetic check uses explicit I_PCM pixels for that slice;
native single-slice and weighted CABAC regressions remain exact. Starting with
I slices and later introducing P references initially needed the replay
implemented in the following step.

### Reference replay and CAVLC PCM alignment

The software path retains unfinished slice payloads and their parsed registers,
with a 64 MiB per-picture bound. Physical reference identities are appended to
a stable list (at most 16). If a later slice introduces a reference, the decoder
is reseeded from guest planes and prior slices are reconstructed with the new
frame number. Ordinary list reorder/subset changes do not replay. Completion,
error and whole-engine reset release retained payloads. Both P-then-I and
I-then-P pictures now pass independently known pixel checks.

CAVLC I_PCM embeds byte-alignment padding within the macroblock payload.
Replacing a slice header can change that alignment even though all entropy
bits remain intact. `h264-cavlc-pcm-offset.patch` adds a private, default-zero
FFmpeg option carrying the original alignment. Its existing CAVLC parser
realigns PCM using a bounded, padded scratch buffer; the transform buffer
cannot be reused because later intra blocks expect it to remain clean.
Seeds and CABAC always use offset zero. CAVLC runs through this software path
because VideoToolbox cannot accept the alignment metadata.

The synthetic sanitizer check covers all eight alignments, PCM following an
ordinary intra block, both slice-type orders, new/reordered/duplicated
references, missing/overlapping macroblocks, reset, and the replay bound.
An independently generated Baseline movie uses four-bit frame numbers, two
slices per picture, and I_PCM/P_PCM blocks for 180 64-by-64 frames. FFmpeg's
ordinary decode and the corrected bridge both match its known NV12 pixels;
the bridge also matches native VideoToolbox decoding through
`test_h264_native.py --software`. Both dependency patches are applied by the
package build and included with its FFmpeg source/build notices.

Packaged run `8350` passes all three complete audio tracks (MP3 resampling
maximum 0.5 S16 step, both AAC tracks exact after amplifier scaling), the final
180-frame weighted CABAC replay, cold-file persistence, USB and normal quit.
Its PCM video produces no frames, exposing a separate guest parser issue.
Trace run `8525` locates this in the short PPS: at `c05b64ac`, 7E18 reads
`0x1480` and compares the result with `0x80000000` to identify trailing bits.
The unimplemented register returned zero, so it parsed nonexistent extension
fields and exhausted input. The register now exposes non-consuming 32-bit
lookahead, padded with zero beyond the input tail. The reader sanitizer check
covers every offset through a full word and a lone trailing stop bit.

Guest repeat `8728` with the lookahead fix decodes all 180 PCM pictures and
then all 180 weighted CABAC pictures. The concatenated NV12 capture matches
both references byte-for-byte, with no bit-reader exhaustion. USB remains
responsive and native PMU shutdown completes successfully.

`tests/ipod/make_h264_pcm_movie.py output.mov` reproduces the PCM fixture
without firmware or game assets and writes independently constructed pixels to
`output.nv12`. It verifies the movie with an ordinary FFmpeg decode. Optional
`--audio source.mov` copies an existing audio track for combined playback tests.

Constrained intra prediction is mapped from register `0x1018`. In 7E18, the
PPS parser reads chroma QP, deblocking-present, constrained-intra and redundant
flags at `c05b644c`–`c05b6490`; the job setup copies the constrained flag from
PPS offset `0x494` into `0x1018` at `c05b2ffc`. The reconstructed PPS preserves
this flag, including retained/replayed slices, and rejects changes within an
incomplete picture. The sanitizer test places an intra DC macroblock beside
an inter macroblock: its luma must be 128 when constrained, versus 80 when
unconstrained. It also checks invalid flag values and mid-picture changes.
A 180-frame, four-reference weighted CABAC fixture with constrained intra
prediction matches native VideoToolbox decoding byte-for-byte.

Guest run `13726` confirms the firmware writes `0x1018 = 1` for this fixture.
All 180 constrained-intra frames followed by all 180 PCM replay frames match
their independent NV12 references exactly. USB stays responsive and the native
PMU halt completes with QEMU exit status zero.

Deblocking offsets in `0x1060` and `0x1064` are twice their signed slice-header
syntax values: 7E18 shifts them at `c05b3778` and `c05b3788`, then writes them
at `c05b311c`–`c05b3134`. The bridge now validates even values from -12 through
12 and divides by two when reconstructing the header. The native comparison
harness also writes actual hardware units. A weighted, four-reference,
multi-slice movie encoded with `-x264-params
'bframes=0:ref=4:weightp=1:deblock=6,-6:slice-max-mbs=70:keyint=180:scenecut=0'`
failed on its first frame before the fix; afterward all 180 frames match
VideoToolbox exactly. Run the comparison with the patched FFmpeg prefix:
`PKG_CONFIG_PATH=build-native14/prefix/lib/pkgconfig python3
tests/ipod/test_h264_native.py movie.mov --software`.

Guest run `15490` writes +12/-12 into the deblocking registers and produces
all 180 offset-test frames exactly, followed by 180 exact constrained-intra
replay frames. USB remains responsive and native PMU shutdown exits cleanly.
The sanitizer check also rejects odd and out-of-range hardware offsets without
writing destination pixels.
