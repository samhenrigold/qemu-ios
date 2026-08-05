# App compatibility survey

A survey of 20 real 2008–2010 App Store apps against this emulator (1st/2nd-gen
iPod touch, iOS 3.1.3). Surveyed 2026-08-05.

The question it answers: **which real apps can run here, and what is missing?**

## Method

Everything below was measured, not assumed:

- **Bundle metadata** — unzip the `.ipa`, read `Payload/*.app/Info.plist`
  (`CFBundleIdentifier`, `CFBundleExecutable`, `DTSDKName`,
  `MinimumOSVersion`).
- **Encryption** — `otool -l <binary>`, field `cryptid` in
  `LC_ENCRYPTION_INFO`. `cryptid 1` means the binary is still FairPlay
  encrypted; decryption needs the purchasing device's key, so such an app
  installs and then never launches. Nothing we implement can change that.
- **Architectures** — `lipo -info` / `file`. This guest is an ARM1176 (armv6);
  an armv7-only slice cannot execute.
- **GL imports** — `nm -u <binary>`, filtered to `_gl*`.
- **Coverage** — those imports cross-referenced against the slots declared in
  `include/hw/arm/guest-services/gles.h` and filled in the guest shim's
  dispatch table in `contrib/it-gles/mbxshim.c`. The framework aliases the
  `OES` and non-`OES` spellings onto one slot, so both count as covered.
- **PVRTC** — `.pvr` files counted, then each file's 52-byte v2 header decoded
  to read the real pixel type. The extension alone is not evidence: a `.pvr`
  container frequently holds uncompressed data (see the footnote on Angry
  Birds).

`DTSDKName` is recorded exactly as found, including Doodle Jump's literal
unexpanded `(SDK_NAME)` — that build never had the variable substituted, which
is itself the tell that it is not a normal App Store package.

## Per-app results

"Missing" = GL entry points the app imports that this emulator does not
implement.

| App | Bundle id | DTSDKName | MinOS | Encrypted | Archs | GL syms | .pvr | VBO | Missing GL |
|---|---|---|---|---|---|---|---|---|---|
| 365XWords | `ssacross.com.simonandschuster` | `iphoneos2.1` | 2.1 | no | armv6 | — | 0 | no | — |
| AngryBirds | `com.clickgamer.AngryBirds` | `iphoneos3.0` | 3.0 | no | armv6 | 51 | 9 [^1] | yes | `glActiveTexture`, `glBufferData`, `glClientActiveTexture`, `glCompressedTexImage2D`, `glDeleteBuffers`, `glDepthFunc`, `glFrontFace`, `glGenBuffers`, `glPixelStorei`, `glScissor`, `glTexEnvi` |
| Classics | `com.classicsapp.classics` | `iphoneos2.0` | 2.0 | no | armv6 | — | 0 | no | — |
| Epicurious | `com.condenet.Epicurious` | `iphoneos2.2` | 2.2 | no | armv6 | — | 0 | no | — |
| Yelp | `com.yelp.yelpiphone` | `iphoneos2.1` | 2.1 | no | armv6 | — | 0 | no | — |
| Cube Runner | `com.andyqua.CubeRunner` | `iphoneos3.0` | 3.0 | no | armv6 | 41 | 0 | no | none |
| DoodleJump | `com.yourcompany.DoodleJump` | `(SDK_NAME)` [^2] | (absent) | no | armv6 | 36 | 0 | no | `glCompressedTexImage2D` [^3], `glScissor`, `glTexEnvi` |
| Facebook | `com.facebook.Facebook` | `iphoneos4.2` | 3.0 | no | armv6 + armv7 | — | 0 | no | — |
| Instagram | `com.burbn.instagram` | `iphoneos5.1` | 3.1.2 | no | armv7 + armv6 | 54 | 10 | no | 28 entry points, all OpenGL ES **2.0** [^4] |
| Tweetie | `com.atebits.Tweetie2` | `iphoneos3.1.3` | 3.0 | no | armv6 | — | 0 | no | — |
| Postman | `com.freeverse.Postman` | `iphoneos3.0` | 3.0 | no | armv6 | — | 0 | no | — |
| MassTransit | `com.sparkfishcreative.masstransit` | `iphoneos4.0` | 3.0 | no | armv6 | — | 0 | no | — |
| Pandora | `com.pandora` | `iphoneos6.1` | 3.0 | no | armv6 + armv7 | — | 0 | no | — |
| Shazam | `com.shazam.Shazam` | `iphoneos2.1` | 2.1 | no | armv6 | — | 0 | no | — |
| Starbucks | `com.starbucks.mystarbucks` | `iphoneos3.0` | 3.0 | no | armv6 | — | 0 | no | — |
| Monkey Ball | `com.ooi.supermonkeyball` | `iphoneos2.0` | 2.0 | no | armv6 | 49 | 0 | no | none |
| 2cute | `com.stuckpixelinc.2cute` | `iphoneos2.0` | 2.0 | **yes** | armv6 | — | 0 | no | — |
| AOL Radio | `com.aol.radio` | `iphoneos2.0` | 2.0 | **yes** | armv6 | — | 0 | no | — |
| Funny Pics | `com.stuckpixelinc.funnypictures` | `iphoneos2.0` | 2.0 | **yes** | armv6 | — | 0 | no | — |
| GuessMyAge | `com.palawin.GuessMyAge` | `iphoneos2.0` | 2.0 | **yes** | armv6 | — | 0 | no | — |

[^1]: Only **4 of the 9** are actually PVRTC. Decoding each header: four are
    PVRTC 4bpp (one 1024×1024 atlas and three mipmapped 256×256), and five are
    pixel type `0x10`, i.e. uncompressed RGBA4444 merely stored in a PVR
    container. So the PVRTC requirement is real but smaller than a file count
    suggests.

[^2]: Recorded verbatim. The build has an unsubstituted `$(SDK_NAME)` and no
    `MinimumOSVersion` at all — consistent with a homebrew or repackaged
    build rather than a store package.

[^3]: Imported but very likely dead. Doodle Jump ships zero `.pvr` files and
    only PNG textures, so this import is probably pulled in by a linked helper
    and never called. Inferred from the bundle contents, not from a trace.

[^4]: Not a coverage gap — a different API. These are shader entry points
    (`glCreateShader`, `glLinkProgram`, `glUniform*`, `glVertexAttribPointer`,
    …). The MBX is fixed-function hardware with no shader support, so this app
    is out of scope permanently, independent of its SDK version.

## Headline numbers

Of 20 apps:

- **16 are decrypted and have an armv6 slice** — the baseline for running at
  all. Every app in the corpus ships armv6; the only hard exclusions are the
  **4 encrypted** ones (`cryptid 1`), which cannot run no matter what we
  implement.
- **12 are clean candidates** — decrypted, armv6, and built against an SDK at
  or below 3.1.3.
- **2 are marginal** — Facebook (SDK 4.2) and MassTransit (SDK 4.0). Both are
  armv6 and declare `MinimumOSVersion` 3.0, so they may well run; untested.
- **2 are decrypted but out of reach** — Instagram (SDK 5.1, and an ES 2.0
  shader app) and Pandora (SDK 6.1).
- **5 use OpenGL at all.** Excluding Instagram, that is **4**: Cube Runner,
  Super Monkey Ball, Doodle Jump, Angry Birds.
- **2 ship `.pvr` files** (Angry Birds, Instagram); only Angry Birds is
  runnable, and only 4 of its 9 files are genuinely PVRTC.
- **1 uses VBOs** (Angry Birds).

`DTSDKName` predicts runnability far better than `MinimumOSVersion`: Facebook,
MassTransit and Pandora all claim `MinimumOSVersion` 3.0 while being built
against the 4.x/6.x SDKs.

## Missing GL functions, ranked

Counting only runnable apps (Instagram's ES 2.0 set excluded):

| Implementing… | unblocks | which |
|---|---|---|
| `glTexEnvi` | 2 apps | Angry Birds, Doodle Jump |
| `glScissor` | 2 apps | Angry Birds, Doodle Jump |
| `glCompressedTexImage2D` | 2 apps | Angry Birds, Doodle Jump (Doodle Jump's is likely dead — see [^3]) |
| `glActiveTexture` | 1 app | Angry Birds |
| `glClientActiveTexture` | 1 app | Angry Birds |
| `glPixelStorei` | 1 app | Angry Birds |
| `glDepthFunc` | 1 app | Angry Birds |
| `glFrontFace` | 1 app | Angry Birds |
| `glGenBuffers` | 1 app | Angry Birds |
| `glBufferData` | 1 app | Angry Birds |
| `glDeleteBuffers` | 1 app | Angry Birds |

Cube Runner and Super Monkey Ball need **nothing** — their import sets are
already fully covered.

## Conclusions

**PVRTC (plan 03) and VBO (plan 04) have exactly one runnable beneficiary
between them: Angry Birds.** It needs both. No other runnable app in the
corpus touches either feature. They are therefore one project or neither —
building only one leaves Angry Birds still broken and benefits nobody else.

**Seven cheap state setters are the better value.** `glPixelStorei`,
`glScissor`, `glTexEnvi`, `glActiveTexture`, `glClientActiveTexture`,
`glDepthFunc`, `glFrontFace` are each wanted by up to 2 apps and are
near-trivial — plain state forwarding with no data path. That work is already
underway.

**The corpus is mostly not a GL problem.** 15 of 20 apps import no GL at all;
for them the gates are encryption and SDK vintage, neither of which is
engineering we can do.

## Runtime status

**Most of this document is static analysis. Runtime behaviour was only spot
-checked, and most candidates have never been launched.**

Verified by running (2026-08-05):

- **Doodle Jump** — installs, launches, and renders. Its menu buttons and
  first-run dialog draw and respond to taps; the background does not draw and
  the in-game screen is white. Consistent with `glTexEnvi` being dropped.
- **Angry Birds** — installs and launches, and actively renders (tens of
  thousands of frames, ~10 draw calls each), but presents a pure white screen.
  The proximate cause is **not** a missing GL function: its CAEAGLLayer
  drawable is RGB565 (fourcc `'L565'`, stride 640 for a 320-wide surface) and
  the surface plausibility check in `contrib/it-gles/mbxshim.c` hardcodes 4
  bytes per pixel (`stride < width * 4`), so the surface is rejected and the
  app draws into a framebuffer with no colour attachment. Plans 03 and 04
  cannot make this app draw a pixel until that check accounts for 16-bit
  drawables.

Not tested at runtime: **the other 14 candidates.** Every claim about them
above is derived from bundle metadata and symbol tables only. In particular,
"decrypted + armv6 + old SDK" is a necessary condition for running, never a
sufficient one — an app can satisfy all three and still fail on a framework,
a device capability, or a service that does not exist here.

Encrypted apps (`cryptid 1`) cannot run regardless of anything we implement,
so they are excluded from every count of candidates above.

### Note for whoever tests the rest

Guest touch becomes unreliable when several emulators run on the host at once:
taps and swipes silently fail while QMP still reports success. A tap that does
not launch an app is not evidence the app is broken — retry before recording a
failure.
