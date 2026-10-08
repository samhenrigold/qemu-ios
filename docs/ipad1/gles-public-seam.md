# The public GL seam: what the firmware takes from OpenGLES.framework

2026-09-29. The survey behind `contrib/gles-public` (one OpenGLES.framework replacement for every emulated iOS).
Static reading of each firmware's shared cache (or, on 2.x, its framework binaries); no emulator runs.
The tables came from `contrib/gles-public/seam_survey.py` (the inputs are the rootfs files named under "Builds");
it is retired, with no replacement (see git history at 5508b504b8).

## Builds

| column | board | iOS | build | read from |
|---|---|---|---|---|
| 5H11a | iPod touch 2G (N72) | 2.2.1 | 5H11a | framework binaries (2.x has no shared cache) + SpringBoard |
| 7E18 | iPod touch 2G | 3.1.3 | 7E18 | dyld_shared_cache_armv6 + SpringBoard |
| n72-8C148 | iPod touch 2G | 4.2.1 | 8C148 | dyld_shared_cache_armv6 + SpringBoard |
| 7B500 | iPad 1 (K48) | 3.2.2 | 7B500 | dyld_shared_cache_armv7 + SpringBoard |
| 8C148 | iPad 1 | 4.2.1 | 8C148 | dyld_shared_cache_armv7 + SpringBoard |
| 8L1 | iPad 1 | 4.3.5 | 8L1 | dyld_shared_cache_armv7 + SpringBoard |
| 9B206 | iPad 1 | 5.1.1 | 9B206 | dyld_shared_cache_armv7 + SpringBoard |

The brief's "3.2 (7B500)" is 3.2.2; 3.2 (7B367) and 3.2.2 share the iPad 3.x family and were not read separately.

## Findings

1. **Who links OpenGLES.** Only these images, on any build: QuartzCore; MapKit; iLifeSlideshow (3.2+); MediaToolbox,
   WebCore (4.x+); PhotoBoothEffects (4.3+); CoreImage, CoreVideo, GLKit, FaceCoreLight, ToneLibrary, OpenCL (5.x).
   **SpringBoard and UIKit import nothing from OpenGLES on any build** (5.1.1's SpringBoard links it and binds no
   symbol from it). SpringBoard reaches GL only through QuartzCore's render server in its process. **WebKit**
   (the framework) does not link it; WebCore does, from 4.2.1 (136 imports, the public ES2 API and EAGL's public
   methods only).
2. **Public vs private.** Every consumer except QuartzCore, CoreImage, CoreVideo, MapKit, MediaToolbox,
   PhotoBoothEffects and iLifeSlideshow uses only public API. The private surface, all of it:
   - EAGL methods: `initWithAPI:properties:` (QuartzCore 3.1+, CoreImage/PhotoBoothEffects 5.x),
     `attachImage:toCoreSurface:invertedRender:` (every one of those seven), `swapNotification:forTransaction:onLayer:`
     (QuartzCore 3.1+), `sendNotification:forTransaction:onLayer:` (QuartzCore 3.2+ and the iPod's 4.2.1),
     `texImageIOSurface:target:internalFormat:width:height:format:type:plane:invert:` (QuartzCore, CoreImage, CoreVideo
     on 5.x), `GetMacroContextPrivate` (QuartzCore, CoreImage on 5.x), `-[EAGLSharegroup APIs]` (GLKit 5.x),
     `-[EAGLSharegroup getGLIShared]` (OpenCL 5.x; nothing on the iPad calls OpenCL).
   - Exports: `EAGLMemoryNotificationRecycling` (QuartzCore 4.x+), `kEAGLContextPropertyAccelerated`,
     `kEAGLContextPropertySharegroup` (QuartzCore 3.1+), `kEAGLContextPropertyClientRetainRelease` (5.x).
     The other private exports (`GLC*Dispatch*`, `GLIContextFromEAGLContext`, `EAGLGetCurrentMacroContextPrivate`,
     `_OBJC_IVAR_$_EAGLContext._private`) are imported by **no** image of any build read.
   - 2.x: QuartzCore drives GL through **EGL** (11 `egl*` imports) plus 44 `gl*` (`glTexImageCoreSurfaceAPPLE` and
     `glFinishTextureAPPLE` among them); it sends no EAGL selector. MapKit (2.x, private) uses public EAGL plus `attachImage:`.
   - Selectors OpenGLES sends back (the drawable side): `nativeWindow`, `drawableProperties` on the layer, and plain
     Foundation messages (`objectForKey:`, `boolValue`, ...). Unchanged 2.2.1 to 5.1.1.
3. **The 5.x compositor path, verified by disassembly (9B206 QuartzCore).** QuartzCore imports no `gl*` at all. Its
   renderer stores `[context GetMacroContextPrivate]` (the only send, a tail call at 0x3268435c; stored at
   0x32684224 to `[renderer, #0x1c4]`) and every GL call goes through that block: `ldr r0, [m]` (the engine's
   context) then `ldr ip, [m, #4 + 4*k]; blx ip`, k a `__GLIFunctionDispatchRec` field. Examples: 0x32662adc
   `[m, #0x800]` is field 511, `vertex_attrib_pointer_ARB`; 0x3266203e/0x32662042 `movw r1, #0x8d40` then
   `[m, #0xa84]`, field 672, `bind_framebuffer_EXT(GL_FRAMEBUFFER, …)`; 0x3266204e `[m, #0xa8c]`,
   `gen_framebuffers_EXT`. So the study's inference holds: on 5.x a replacement must answer `GetMacroContextPrivate`
   with `{GC, dispatch table}` laid out as this firmware's `__GLIFunctionDispatchRec` (905 fields on 9B206), each
   entry called with the GC as its first argument. CoreImage does the same.
   The @encode that names the fields is carried by **OpenGLES alone** (3.x: `__cstring`; 4.x/5.x:
   `__objc_methtype`) on every build read, so a replacement reads it from the stock OpenGLES image in the mapped
   shared cache, not from a loaded image.
4. **Stock EAGL's private methods, what they do (8C148, same on 7B500 and 9B206):**
   - `swapNotification:fb forTransaction:t onLayer:l`: an accelerated context sends GLI parameter 0x2C1
     `{IOMobileFramebufferGetID(fb), t, l}` to its engine (the GPU completes the swap); an unaccelerated one calls
     `IOMobileFramebufferSwapSignal(fb, t, l)` itself (8C148 0x3555737c, stub resolved).
   - `sendNotification:id forTransaction:t onLayer:l`: the same 0x2C1 with an ID the caller already has; an
     unaccelerated context does nothing.
   - The 3.1.3 iPod's MBX engine path is the same message through the engine interface's +0x20
     (`GLESSwapNotification`).
5. **The native window.** `-[CAEAGLLayer nativeWindow]` returns `_EAGLNativeWindowObject`
   `{version, attach, detach, begin, swap, collect}` on 3.x (2.x: the same six words, unnamed), plus a seventh,
   `properties`, on 4.2.1, 4.3.5 and 5.1.1 (5.1.1 unnamed, seven words). The callbacks block the engine hands to
   `attach` is `EAGLNativeWindowCallbacksRec {callback_data, create_buffer, destroy_buffer}` on 3.x (named on 7E18
   and 7B500); 4.x's stock engine adds a fourth word, the preflight callback (`contrib/ipad1-gles/glishim.c`
   gli_bind_view4). A block with the fourth word is read correctly by the three-word consumers.
6. **What changes across builds at this seam** (tables below): 2.x → 3.x drops EGL and adds the GLI/GLC exports
   and `initWithAPI:properties:`; 3.1.3 → 3.2.2 adds `sendNotification:`; 3.x → 4.2.1 adds
   `texImageIOSurface:`, `EAGLMemoryNotificationRecycling`, the window's `properties`; **4.2.1 → 4.3.5 changes
   nothing** (318 identical exports, the same methods and structures); 4.3.5 → 5.1.1 adds `GetMacroContextPrivate`,
   `EAGLGetCurrentMacroContextPrivate`, `kEAGLContextPropertyClientRetainRelease`, the compute variants and 36 `gl*`
   (EXT_separate_shader_objects, debug label/marker, occlusion queries, map_buffer_range, ...). Every such
   difference is visible to a loaded binary (a selector sent or not, a struct's version word, a symbol exported),
   so none needs a per-build table.

## What the replacement must find at run time (and the prepare's fit check proves)

| piece | where it is looked up | needed when |
|---|---|---|
| every name the stock OpenGLES exports | the binary's export list vs. the firmware's OpenGLES (dyld binds every cached consumer's imports by name once the cached image is overridden) | always |
| `nativeWindow` + `drawableProperties` on CAEAGLLayer | QuartzCore implements both | an app renders to a layer (all builds) |
| the window object's shape | its `version` word at bind; `properties` only when version > 1 | as above |
| IOSurface (3.x+) or CoreSurface (2.x) surface calls | `dlsym` by name in whichever framework the firmware has | attach / texImageIOSurface / present |
| `IOMobileFramebufferSwapSignal`, `…GetID`, `…GetMainDisplay` | `dlsym` in IOMobileFramebuffer | QuartzCore sends swap/sendNotification (3.1+) |
| the `__GLIFunctionDispatchRec` @encode, every field known to `gles-names.h` | the stock OpenGLES image in the mapped shared cache (`shared_region_check_np`, the cache's image list) | an image sends `GetMacroContextPrivate` (5.x) |
| ES 2.0 | `OpenGLES.framework/GLEngine.bundle` (the SGX engine) exists; the iPod's firmware has only `MBXGLEngine.bundle` / none | an app asks for `kEAGLRenderingAPIOpenGLES2` |

## Result (gles-public, 2026-09-29/30)

`contrib/gles-public/OpenGLES`, sha256 `e5590426bdf81570b15ef2e50a369751082f9daf7511feeb79627b682ca2d25b`, the same
file on every build below. Prepared through FirmwareKit (`FitCheck.glesFrontEnd` required) and booted through the app's
pipeline (LightTouchMac `tests/sessions/matrix.py`, with the Harness GL row tapped via `--gl-tap`). Host load was 50 to
200 throughout, from other agents' emulators.

| build | GL path in the log (the front end's line) | GL app | notes |
|---|---|---|---|
| iPad 3.2 7B367 | `attachImage:toCoreSurface:invertedRender:` | Harness GL: triangle drawn | every column but boot-2 shutdown, the recorded slide-sheet flake |
| iPad 3.2.2 7B500 | same | Harness GL drawn (29 fps) | every column but boot-2 shutdown (the flake); a rerun hit the AppleBCMWLAN panic after joining Wi-Fi, not GL |
| iPad 4.2.1 8C148 | same | Harness GL drawn (30 fps); `it_gltest` readback PASS, scene composited, 61.5 presents/s | every column; home 0.0000 against its matrix ref |
| iPad 4.3.5 8L1 | same | Harness GL drawn | every column but boot-2 shutdown (the flake); home 0.0021 against its ref. Under glishim this row was software CA (gl FAIL) |
| iPad 5.1.1 9B206 | `macro context` (905 fields from the shared cache's stock OpenGLES, all named) | `it_gltest` scene composited | Setup Assistant composited. Home was dark in the one run that lit: its first composite came late at load 90+. The fixture's `glGenTextures` into an untouched page is dropped identically under glishim (LightTouchMac smoke #69) |
| iPod 2.2.1 5H11a | `egl: first pixmap surface` | PAC-MAN Lite title screen | every column |
| iPod 3.1.3 7E18 | `attachImage:…` | Harness GL drawn (28 fps) | every column |
| iPod 4.2.1 8C148 | same | Harness GL drawn (27.7 fps) | every column |

On 4.2.1 with the fixture, glishim and the front end A/B'd back to back gave the same tearing (0 torn, 0.23 to 0.25
partial) and the same present rate within the load noise (15 to 20 per second each).

## 1.x (iPod touch 1G, 1.1-1.1.5): not in the one binary

1.x has LayerKit, not CoreAnimation, and no EAGL; its Objective-C is the old runtime (CoreFoundation exports
`.objc_class_name_NSObject`, libobjc has no `objc_msgSendSuper2`). `contrib/it-gles/gles1x.c` built as plain C (`OpenGLES-1x`, 186 exports, guest package `n45-ios1`) keeps serving it. Folding it into
`contrib/gles-public/OpenGLES` as a second backend chosen at run time is not cheap with the front end as it is. These are
the pieces:

1. The binary's ObjC 2 bindings (`_OBJC_CLASS_$_NSObject`, `___CFConstantStringClassReference`, `_objc_msgSendSuper2`,
   `__objc_empty_cache`) would have to become weak references, so 1.x's dyld loads the image with them NULL. That
   weakens `FitCheck.loads` on 2.x-5.x, where they must bind: the fit check would need an explicit "binds NSObject from
   CoreFoundation" proof instead.
2. Unknown: whether 1.x's old-ABI libobjc really ignores a `__DATA,__objc_classlist` image, as it should. It reads
   `__OBJC` only.
3. The two 1.x-only exports, `eglSwapNotification` and `glVertexAttribPointerARB`, added to `opengles.exports`, and
   1.x's no-code-signing rule (an `LC_CODE_SIGNATURE` it doesn't know, which has no REQ_DYLD bit) proven harmless.
4. `FitCheck.glesFrontEnd` taught LayerKit as the drawable owner where there is no QuartzCore, and `N45Recipe` moved
   from `frontEnd`'s exact-export match to the fit check.
5. Gate: the n45 GL regress leg (LayerKit composites through the front end) and the n45 matrix rows. Their lockdown,
   AFC and shutdown columns fail today for USB reasons (smoke #43), unrelated to GL.

## Tables

Generated by `seam_survey.py --markdown` from the builds above. "kind": `gl core` / `gl extension` are Khronos
names (an OES/EXT/APPLE/IMG suffix is an extension), `EAGL class` / `EAGL constant` / `EAGL function` are the
public EAGL API, `EGL` is the 1.x/2.x EGL API, `private` is everything else. EAGL selectors are public when the
SDK headers of their build declare them (EAGL.h, EAGLDrawable.h).

## Symbols and selectors other images take from OpenGLES

Cells: which images use it on that build (blank: none).

| symbol / selector | kind | 5H11a | 7E18 | n72-8C148 | 7B500 | 8C148 | 8L1 | 9B206 |
|---|---|---|---|---|---|---|---|---|
| `_OBJC_CLASS_$_EAGLContext` | EAGL class | MapKit | MapKit, QuartzCore | MapKit, MediaToolbox, QuartzCore | MapKit, QuartzCore, iLifeSlideshow | MapKit, MediaToolbox, QuartzCore, WebCore, iLifeSlideshow | MapKit, MediaToolbox, PhotoBoothEffects, QuartzCore, WebCore, iLifeSlideshow | CoreImage, FaceCoreLight, GLKit, MediaToolbox, PhotoBoothEffects, QuartzCore, ToneLibrary, WebCore, iLifeSlideshow |
| `_OBJC_CLASS_$_EAGLSharegroup` | EAGL class |  | QuartzCore | QuartzCore | QuartzCore | QuartzCore | QuartzCore |  |
| `_kEAGLColorFormatRGB565` | EAGL constant |  |  |  |  |  |  | GLKit |
| `_kEAGLColorFormatRGBA8` | EAGL constant |  |  |  | iLifeSlideshow | iLifeSlideshow | iLifeSlideshow | GLKit, ToneLibrary, iLifeSlideshow |
| `_kEAGLContextPropertyAccelerated` | EAGL constant |  | QuartzCore | QuartzCore | QuartzCore | QuartzCore | QuartzCore | CoreImage, QuartzCore |
| `_kEAGLContextPropertyClientRetainRelease` | EAGL constant |  |  |  |  |  |  | CoreImage, PhotoBoothEffects, QuartzCore |
| `_kEAGLContextPropertySharegroup` | EAGL constant |  | QuartzCore | QuartzCore | QuartzCore | QuartzCore | QuartzCore | CoreImage, PhotoBoothEffects, QuartzCore |
| `_kEAGLDrawablePropertyColorFormat` | EAGL constant |  |  |  | iLifeSlideshow | iLifeSlideshow | iLifeSlideshow | GLKit, ToneLibrary, iLifeSlideshow |
| `_kEAGLDrawablePropertyRetainedBacking` | EAGL constant |  |  |  | iLifeSlideshow | iLifeSlideshow | iLifeSlideshow | GLKit, ToneLibrary, iLifeSlideshow |
| `_eglChooseConfig` | EGL | QuartzCore |  |  |  |  |  |  |
| `_eglCreateContext` | EGL | QuartzCore |  |  |  |  |  |  |
| `_eglCreatePixmapSurface` | EGL | QuartzCore |  |  |  |  |  |  |
| `_eglDestroyContext` | EGL | QuartzCore |  |  |  |  |  |  |
| `_eglDestroySurface` | EGL | QuartzCore |  |  |  |  |  |  |
| `_eglEnableInternalSurface` | EGL | QuartzCore |  |  |  |  |  |  |
| `_eglGetConfigAttrib` | EGL | QuartzCore |  |  |  |  |  |  |
| `_eglGetDisplay` | EGL | QuartzCore |  |  |  |  |  |  |
| `_eglGetError` | EGL | QuartzCore |  |  |  |  |  |  |
| `_eglInitialize` | EGL | QuartzCore |  |  |  |  |  |  |
| `_eglMakeCurrent` | EGL | QuartzCore |  |  |  |  |  |  |
| `_glActiveTexture` | gl core | MapKit, QuartzCore | MapKit, QuartzCore | MapKit, QuartzCore | MapKit, QuartzCore, iLifeSlideshow | MapKit, QuartzCore, WebCore, iLifeSlideshow | MapKit, PhotoBoothEffects, QuartzCore, WebCore, iLifeSlideshow | CoreVideo, GLKit, PhotoBoothEffects, ToneLibrary, WebCore, iLifeSlideshow |
| `_glAttachShader` | gl core |  | QuartzCore |  | QuartzCore | QuartzCore, WebCore | PhotoBoothEffects, QuartzCore, WebCore | GLKit, PhotoBoothEffects, ToneLibrary, WebCore |
| `_glBindAttribLocation` | gl core |  | QuartzCore |  | QuartzCore | QuartzCore, WebCore | PhotoBoothEffects, QuartzCore, WebCore | GLKit, PhotoBoothEffects, ToneLibrary, WebCore |
| `_glBindBuffer` | gl core | MapKit | MapKit, QuartzCore | MapKit | MapKit, QuartzCore | MapKit, QuartzCore, WebCore | MapKit, QuartzCore, WebCore | GLKit, WebCore |
| `_glBindFramebuffer` | gl core |  | QuartzCore | MediaToolbox | QuartzCore | MediaToolbox, QuartzCore, WebCore | MediaToolbox, PhotoBoothEffects, QuartzCore, WebCore | GLKit, MediaToolbox, PhotoBoothEffects, ToneLibrary, WebCore |
| `_glBindRenderbuffer` | gl core |  |  |  |  | WebCore | WebCore | GLKit, ToneLibrary, WebCore |
| `_glBindTexture` | gl core | MapKit, QuartzCore | MapKit, QuartzCore | MapKit, MediaToolbox, QuartzCore | MapKit, QuartzCore, iLifeSlideshow | MapKit, MediaToolbox, QuartzCore, WebCore, iLifeSlideshow | MapKit, MediaToolbox, PhotoBoothEffects, QuartzCore, WebCore, iLifeSlideshow | CoreVideo, FaceCoreLight, GLKit, MediaToolbox, PhotoBoothEffects, ToneLibrary, WebCore, iLifeSlideshow |
| `_glBlendColor` | gl core |  |  |  |  | WebCore | WebCore | WebCore |
| `_glBlendEquation` | gl core |  |  |  |  | QuartzCore, WebCore | QuartzCore, WebCore | WebCore |
| `_glBlendEquationSeparate` | gl core |  |  |  |  | WebCore | WebCore | WebCore |
| `_glBlendFunc` | gl core | MapKit, QuartzCore | MapKit, QuartzCore | MapKit, QuartzCore | MapKit, QuartzCore, iLifeSlideshow | MapKit, QuartzCore, WebCore, iLifeSlideshow | MapKit, QuartzCore, WebCore, iLifeSlideshow | ToneLibrary, WebCore, iLifeSlideshow |
| `_glBlendFuncSeparate` | gl core |  |  |  |  | WebCore | WebCore | WebCore |
| `_glBufferData` | gl core | MapKit | MapKit, QuartzCore | MapKit | MapKit, QuartzCore | MapKit, QuartzCore, WebCore | MapKit, QuartzCore, WebCore | GLKit, WebCore |
| `_glBufferSubData` | gl core |  |  |  |  | WebCore | WebCore | WebCore |
| `_glCheckFramebufferStatus` | gl core |  |  |  |  | WebCore | PhotoBoothEffects, WebCore | GLKit, PhotoBoothEffects, ToneLibrary, WebCore |
| `_glClear` | gl core | MapKit, QuartzCore | MapKit, QuartzCore | MapKit, MediaToolbox, QuartzCore | MapKit, QuartzCore, iLifeSlideshow | MapKit, MediaToolbox, QuartzCore, WebCore, iLifeSlideshow | MapKit, MediaToolbox, QuartzCore, WebCore, iLifeSlideshow | FaceCoreLight, MediaToolbox, ToneLibrary, WebCore, iLifeSlideshow |
| `_glClearColor` | gl core | MapKit, QuartzCore | MapKit, QuartzCore | MapKit, MediaToolbox, QuartzCore | MapKit, QuartzCore, iLifeSlideshow | MapKit, MediaToolbox, QuartzCore, WebCore, iLifeSlideshow | MapKit, MediaToolbox, QuartzCore, WebCore, iLifeSlideshow | FaceCoreLight, MediaToolbox, ToneLibrary, WebCore, iLifeSlideshow |
| `_glClearDepthf` | gl core |  |  |  |  | WebCore | WebCore | WebCore |
| `_glClearStencil` | gl core |  |  |  |  | WebCore | WebCore | WebCore |
| `_glClientActiveTexture` | gl core | MapKit, QuartzCore | MapKit, QuartzCore | MapKit, QuartzCore | MapKit, iLifeSlideshow | MapKit, iLifeSlideshow | MapKit, iLifeSlideshow | iLifeSlideshow |
| `_glColor4f` | gl core | MapKit, QuartzCore | MapKit | MapKit | MapKit, iLifeSlideshow | MapKit, iLifeSlideshow | MapKit, iLifeSlideshow | iLifeSlideshow |
| `_glColor4ub` | gl core | QuartzCore |  |  |  |  |  |  |
| `_glColorMask` | gl core |  |  |  |  | WebCore | WebCore | WebCore |
| `_glColorPointer` | gl core | MapKit, QuartzCore | MapKit, QuartzCore | MapKit, QuartzCore | MapKit, iLifeSlideshow | MapKit, iLifeSlideshow | MapKit, iLifeSlideshow | iLifeSlideshow |
| `_glCompileShader` | gl core |  | QuartzCore |  | QuartzCore | QuartzCore, WebCore | PhotoBoothEffects, QuartzCore, WebCore | GLKit, PhotoBoothEffects, ToneLibrary, WebCore |
| `_glCompressedTexImage2D` | gl core |  |  |  |  |  |  | GLKit |
| `_glCopyTexImage2D` | gl core |  |  |  |  | WebCore | WebCore | WebCore |
| `_glCopyTexSubImage2D` | gl core |  |  |  |  | WebCore | WebCore | WebCore |
| `_glCreateProgram` | gl core |  | QuartzCore |  | QuartzCore | QuartzCore, WebCore | PhotoBoothEffects, QuartzCore, WebCore | GLKit, PhotoBoothEffects, ToneLibrary, WebCore |
| `_glCreateShader` | gl core |  | QuartzCore |  | QuartzCore | QuartzCore, WebCore | PhotoBoothEffects, QuartzCore, WebCore | GLKit, PhotoBoothEffects, ToneLibrary, WebCore |
| `_glCullFace` | gl core | QuartzCore |  |  | iLifeSlideshow | WebCore, iLifeSlideshow | WebCore, iLifeSlideshow | WebCore, iLifeSlideshow |
| `_glDeleteBuffers` | gl core | MapKit | MapKit, QuartzCore | MapKit | MapKit, QuartzCore | MapKit, QuartzCore, WebCore | MapKit, QuartzCore, WebCore | FaceCoreLight, GLKit, WebCore |
| `_glDeleteFramebuffers` | gl core |  | QuartzCore |  | QuartzCore | QuartzCore, WebCore | QuartzCore, WebCore | GLKit, ToneLibrary, WebCore |
| `_glDeleteProgram` | gl core |  | QuartzCore |  | QuartzCore | QuartzCore, WebCore | QuartzCore, WebCore | GLKit, ToneLibrary, WebCore |
| `_glDeleteRenderbuffers` | gl core |  |  |  |  | WebCore | WebCore | GLKit, ToneLibrary, WebCore |
| `_glDeleteShader` | gl core |  | QuartzCore |  | QuartzCore | QuartzCore, WebCore | QuartzCore, WebCore | GLKit, ToneLibrary, WebCore |
| `_glDeleteTextures` | gl core | MapKit, QuartzCore | MapKit, QuartzCore | MapKit, MediaToolbox, QuartzCore | MapKit, QuartzCore, iLifeSlideshow | MapKit, MediaToolbox, QuartzCore, WebCore, iLifeSlideshow | MapKit, MediaToolbox, QuartzCore, WebCore, iLifeSlideshow | CoreVideo, FaceCoreLight, GLKit, MediaToolbox, ToneLibrary, WebCore, iLifeSlideshow |
| `_glDepthFunc` | gl core |  |  |  |  | WebCore | WebCore | WebCore |
| `_glDepthMask` | gl core |  |  |  |  | WebCore | WebCore | WebCore |
| `_glDepthRangef` | gl core |  |  |  |  | WebCore | WebCore | WebCore |
| `_glDetachShader` | gl core |  |  |  |  | WebCore | WebCore | WebCore |
| `_glDisable` | gl core | MapKit, QuartzCore | MapKit, QuartzCore | MapKit, QuartzCore | MapKit, QuartzCore, iLifeSlideshow | MapKit, QuartzCore, WebCore, iLifeSlideshow | MapKit, QuartzCore, WebCore, iLifeSlideshow | WebCore, iLifeSlideshow |
| `_glDisableClientState` | gl core | MapKit, QuartzCore | MapKit, QuartzCore | MapKit, QuartzCore | MapKit, iLifeSlideshow | MapKit, iLifeSlideshow | MapKit, iLifeSlideshow | iLifeSlideshow |
| `_glDisableVertexAttribArray` | gl core |  | QuartzCore |  | QuartzCore | QuartzCore, WebCore | QuartzCore, WebCore | WebCore |
| `_glDrawArrays` | gl core | MapKit, QuartzCore | MapKit, QuartzCore | MapKit, QuartzCore | MapKit, QuartzCore, iLifeSlideshow | MapKit, QuartzCore, WebCore, iLifeSlideshow | MapKit, PhotoBoothEffects, QuartzCore, WebCore, iLifeSlideshow | FaceCoreLight, GLKit, PhotoBoothEffects, ToneLibrary, WebCore, iLifeSlideshow |
| `_glDrawElements` | gl core | MapKit, QuartzCore | MapKit, QuartzCore | MapKit, QuartzCore | MapKit, QuartzCore | MapKit, QuartzCore, WebCore | MapKit, QuartzCore, WebCore | WebCore |
| `_glEnable` | gl core | MapKit, QuartzCore | MapKit, QuartzCore | MapKit, QuartzCore | MapKit, QuartzCore, iLifeSlideshow | MapKit, QuartzCore, WebCore, iLifeSlideshow | MapKit, QuartzCore, WebCore, iLifeSlideshow | FaceCoreLight, ToneLibrary, WebCore, iLifeSlideshow |
| `_glEnableClientState` | gl core | MapKit, QuartzCore | MapKit, QuartzCore | MapKit, QuartzCore | MapKit, iLifeSlideshow | MapKit, iLifeSlideshow | MapKit, iLifeSlideshow | FaceCoreLight, iLifeSlideshow |
| `_glEnableVertexAttribArray` | gl core |  | QuartzCore |  | QuartzCore | QuartzCore, WebCore | PhotoBoothEffects, QuartzCore, WebCore | GLKit, PhotoBoothEffects, ToneLibrary, WebCore |
| `_glFinish` | gl core | MapKit, QuartzCore | MapKit, QuartzCore | MapKit, MediaToolbox, QuartzCore | MapKit, QuartzCore | MapKit, MediaToolbox, QuartzCore, WebCore | MapKit, MediaToolbox, PhotoBoothEffects, QuartzCore, WebCore | GLKit, MediaToolbox, PhotoBoothEffects, WebCore |
| `_glFlush` | gl core | MapKit, QuartzCore | MapKit, QuartzCore | MapKit, MediaToolbox, QuartzCore | MapKit, QuartzCore | MapKit, MediaToolbox, QuartzCore, WebCore | MapKit, MediaToolbox, QuartzCore, WebCore | GLKit, MediaToolbox, PhotoBoothEffects, WebCore |
| `_glFramebufferRenderbuffer` | gl core |  |  |  |  | WebCore | WebCore | GLKit, ToneLibrary, WebCore |
| `_glFramebufferTexture2D` | gl core |  | QuartzCore | MediaToolbox | QuartzCore | MediaToolbox, QuartzCore, WebCore | MediaToolbox, PhotoBoothEffects, QuartzCore, WebCore | MediaToolbox, PhotoBoothEffects, WebCore |
| `_glFrontFace` | gl core | QuartzCore |  |  | iLifeSlideshow | WebCore, iLifeSlideshow | WebCore, iLifeSlideshow | WebCore, iLifeSlideshow |
| `_glFrustumf` | gl core | MapKit | MapKit | MapKit | MapKit, iLifeSlideshow | MapKit, iLifeSlideshow | MapKit, iLifeSlideshow | iLifeSlideshow |
| `_glGenBuffers` | gl core | MapKit | MapKit, QuartzCore | MapKit | MapKit, QuartzCore | MapKit, QuartzCore, WebCore | MapKit, QuartzCore, WebCore | GLKit, WebCore |
| `_glGenFramebuffers` | gl core |  | QuartzCore | MediaToolbox | QuartzCore | MediaToolbox, QuartzCore, WebCore | MediaToolbox, PhotoBoothEffects, QuartzCore, WebCore | GLKit, MediaToolbox, PhotoBoothEffects, ToneLibrary, WebCore |
| `_glGenRenderbuffers` | gl core |  |  |  |  | WebCore | WebCore | GLKit, ToneLibrary, WebCore |
| `_glGenTextures` | gl core | MapKit, QuartzCore | MapKit, QuartzCore | MapKit, MediaToolbox, QuartzCore | MapKit, QuartzCore, iLifeSlideshow | MapKit, MediaToolbox, QuartzCore, WebCore, iLifeSlideshow | MapKit, MediaToolbox, PhotoBoothEffects, QuartzCore, WebCore, iLifeSlideshow | CoreVideo, FaceCoreLight, GLKit, MediaToolbox, PhotoBoothEffects, ToneLibrary, WebCore, iLifeSlideshow |
| `_glGenerateMipmap` | gl core |  |  |  |  | QuartzCore, WebCore | QuartzCore, WebCore | GLKit, WebCore |
| `_glGetActiveAttrib` | gl core |  |  |  |  | WebCore | WebCore | WebCore |
| `_glGetActiveUniform` | gl core |  |  |  |  | WebCore | WebCore | WebCore |
| `_glGetAttachedShaders` | gl core |  |  |  |  |  |  | WebCore |
| `_glGetAttribLocation` | gl core |  |  |  |  | WebCore | WebCore | GLKit, WebCore |
| `_glGetBooleanv` | gl core |  |  |  |  | WebCore | WebCore | WebCore |
| `_glGetBufferParameteriv` | gl core |  |  |  |  | WebCore | WebCore | WebCore |
| `_glGetError` | gl core | QuartzCore | QuartzCore | QuartzCore | QuartzCore | QuartzCore, WebCore | QuartzCore, WebCore | GLKit, WebCore |
| `_glGetFloatv` | gl core | QuartzCore |  |  | iLifeSlideshow | WebCore, iLifeSlideshow | WebCore, iLifeSlideshow | WebCore, iLifeSlideshow |
| `_glGetFramebufferAttachmentParameteriv` | gl core |  |  |  |  | WebCore | WebCore | WebCore |
| `_glGetIntegerv` | gl core | MapKit, QuartzCore | QuartzCore | QuartzCore | QuartzCore, iLifeSlideshow | QuartzCore, WebCore, iLifeSlideshow | QuartzCore, WebCore, iLifeSlideshow | CoreVideo, FaceCoreLight, GLKit, WebCore, iLifeSlideshow |
| `_glGetProgramInfoLog` | gl core |  |  |  |  | WebCore | PhotoBoothEffects, WebCore | GLKit, PhotoBoothEffects, WebCore |
| `_glGetProgramiv` | gl core |  |  |  |  | WebCore | PhotoBoothEffects, WebCore | GLKit, PhotoBoothEffects, ToneLibrary, WebCore |
| `_glGetRenderbufferParameteriv` | gl core |  |  |  |  | WebCore | WebCore | GLKit, ToneLibrary, WebCore |
| `_glGetShaderInfoLog` | gl core |  |  |  |  | WebCore | PhotoBoothEffects, WebCore | GLKit, PhotoBoothEffects, WebCore |
| `_glGetShaderSource` | gl core |  |  |  |  | WebCore | WebCore |  |
| `_glGetShaderiv` | gl core |  |  |  |  | WebCore | PhotoBoothEffects, WebCore | GLKit, PhotoBoothEffects, ToneLibrary, WebCore |
| `_glGetString` | gl core | QuartzCore | MapKit, QuartzCore | MapKit, QuartzCore | MapKit, QuartzCore | MapKit, QuartzCore, WebCore | MapKit, QuartzCore, WebCore | WebCore |
| `_glGetTexParameteriv` | gl core |  |  |  |  | WebCore | WebCore | WebCore |
| `_glGetUniformLocation` | gl core |  | QuartzCore |  | QuartzCore | QuartzCore, WebCore | PhotoBoothEffects, QuartzCore, WebCore | GLKit, PhotoBoothEffects, ToneLibrary, WebCore |
| `_glGetUniformfv` | gl core |  |  |  |  | WebCore | WebCore | WebCore |
| `_glGetUniformiv` | gl core |  |  |  |  | WebCore | WebCore | WebCore |
| `_glGetVertexAttribPointerv` | gl core |  |  |  |  | WebCore | WebCore | WebCore |
| `_glGetVertexAttribfv` | gl core |  |  |  |  | WebCore | WebCore |  |
| `_glGetVertexAttribiv` | gl core |  |  |  |  | WebCore | WebCore |  |
| `_glHint` | gl core |  |  |  |  | WebCore | WebCore | WebCore |
| `_glIsBuffer` | gl core |  |  |  |  | WebCore | WebCore | WebCore |
| `_glIsEnabled` | gl core |  |  |  |  | WebCore | WebCore | WebCore |
| `_glIsFramebuffer` | gl core |  |  |  |  | WebCore | WebCore | WebCore |
| `_glIsProgram` | gl core |  |  |  |  | WebCore | WebCore | WebCore |
| `_glIsRenderbuffer` | gl core |  |  |  |  | WebCore | WebCore | WebCore |
| `_glIsShader` | gl core |  |  |  |  | WebCore | WebCore | WebCore |
| `_glIsTexture` | gl core |  |  |  |  | WebCore | WebCore | WebCore |
| `_glLineWidth` | gl core |  |  |  |  | WebCore | WebCore | WebCore |
| `_glLinkProgram` | gl core |  | QuartzCore |  | QuartzCore | QuartzCore, WebCore | PhotoBoothEffects, QuartzCore, WebCore | GLKit, PhotoBoothEffects, ToneLibrary, WebCore |
| `_glLoadIdentity` | gl core | MapKit, QuartzCore | MapKit, QuartzCore | MapKit, QuartzCore | MapKit, iLifeSlideshow | MapKit, iLifeSlideshow | MapKit, iLifeSlideshow | FaceCoreLight, iLifeSlideshow |
| `_glLoadMatrixf` | gl core | QuartzCore | QuartzCore | QuartzCore | iLifeSlideshow | iLifeSlideshow | iLifeSlideshow | iLifeSlideshow |
| `_glMatrixMode` | gl core | MapKit, QuartzCore | MapKit, QuartzCore | MapKit, QuartzCore | MapKit, iLifeSlideshow | MapKit, iLifeSlideshow | MapKit, iLifeSlideshow | FaceCoreLight, iLifeSlideshow |
| `_glMultMatrixf` | gl core | MapKit | MapKit | MapKit | MapKit, iLifeSlideshow | MapKit, iLifeSlideshow | MapKit, iLifeSlideshow | iLifeSlideshow |
| `_glNormal3f` | gl core |  |  |  | iLifeSlideshow | iLifeSlideshow | iLifeSlideshow | iLifeSlideshow |
| `_glOrthof` | gl core | MapKit, QuartzCore | MapKit | MapKit | MapKit | MapKit | MapKit | FaceCoreLight |
| `_glPixelStorei` | gl core |  |  | QuartzCore | QuartzCore | QuartzCore, WebCore | QuartzCore, WebCore | GLKit, WebCore |
| `_glPolygonOffset` | gl core |  |  |  |  | WebCore | WebCore | WebCore |
| `_glPopMatrix` | gl core | MapKit, QuartzCore | MapKit | MapKit | MapKit, iLifeSlideshow | MapKit, iLifeSlideshow | MapKit, iLifeSlideshow | iLifeSlideshow |
| `_glPushMatrix` | gl core | MapKit, QuartzCore | MapKit | MapKit | MapKit, iLifeSlideshow | MapKit, iLifeSlideshow | MapKit, iLifeSlideshow | iLifeSlideshow |
| `_glReadPixels` | gl core |  |  |  |  | WebCore | WebCore | FaceCoreLight, GLKit, WebCore |
| `_glRenderbufferStorage` | gl core |  |  |  |  | WebCore | WebCore | GLKit, WebCore |
| `_glRotatef` | gl core | MapKit, QuartzCore | MapKit | MapKit | MapKit, iLifeSlideshow | MapKit, iLifeSlideshow | MapKit, iLifeSlideshow | iLifeSlideshow |
| `_glSampleCoverage` | gl core |  |  |  |  | WebCore | WebCore | WebCore |
| `_glScalef` | gl core | MapKit, QuartzCore | MapKit, QuartzCore | MapKit, QuartzCore | MapKit, iLifeSlideshow | MapKit, iLifeSlideshow | MapKit, iLifeSlideshow | iLifeSlideshow |
| `_glScissor` | gl core | QuartzCore | QuartzCore | QuartzCore | QuartzCore | QuartzCore, WebCore | QuartzCore, WebCore | WebCore |
| `_glShaderSource` | gl core |  | QuartzCore |  | QuartzCore | QuartzCore, WebCore | PhotoBoothEffects, QuartzCore, WebCore | GLKit, PhotoBoothEffects, ToneLibrary, WebCore |
| `_glStencilFunc` | gl core |  |  |  |  | WebCore | WebCore | WebCore |
| `_glStencilFuncSeparate` | gl core |  |  |  |  | WebCore | WebCore | WebCore |
| `_glStencilMask` | gl core |  |  |  |  | WebCore | WebCore | WebCore |
| `_glStencilMaskSeparate` | gl core |  |  |  |  | WebCore | WebCore | WebCore |
| `_glStencilOp` | gl core |  |  |  |  | WebCore | WebCore | WebCore |
| `_glStencilOpSeparate` | gl core |  |  |  |  | WebCore | WebCore | WebCore |
| `_glTexCoordPointer` | gl core | MapKit, QuartzCore | MapKit, QuartzCore | MapKit, QuartzCore | MapKit, iLifeSlideshow | MapKit, iLifeSlideshow | MapKit, iLifeSlideshow | FaceCoreLight, iLifeSlideshow |
| `_glTexEnvf` | gl core |  |  |  |  |  |  | FaceCoreLight |
| `_glTexEnvfv` | gl core | MapKit, QuartzCore | MapKit, QuartzCore | MapKit, QuartzCore | MapKit | MapKit | MapKit |  |
| `_glTexEnvi` | gl core | MapKit, QuartzCore | MapKit, QuartzCore | MapKit, QuartzCore | MapKit | MapKit | MapKit |  |
| `_glTexImage2D` | gl core | MapKit, QuartzCore | MapKit, QuartzCore | MapKit, QuartzCore | MapKit, QuartzCore, iLifeSlideshow | MapKit, QuartzCore, WebCore, iLifeSlideshow | MapKit, PhotoBoothEffects, QuartzCore, WebCore, iLifeSlideshow | CoreVideo, FaceCoreLight, GLKit, PhotoBoothEffects, ToneLibrary, WebCore, iLifeSlideshow |
| `_glTexParameterf` | gl core |  | QuartzCore | QuartzCore | QuartzCore | QuartzCore, WebCore | QuartzCore, WebCore | ToneLibrary, WebCore |
| `_glTexParameteri` | gl core | MapKit, QuartzCore | MapKit, QuartzCore | MapKit, MediaToolbox, QuartzCore | MapKit, QuartzCore, iLifeSlideshow | MapKit, MediaToolbox, QuartzCore, WebCore, iLifeSlideshow | MapKit, MediaToolbox, PhotoBoothEffects, QuartzCore, WebCore, iLifeSlideshow | CoreVideo, FaceCoreLight, GLKit, MediaToolbox, PhotoBoothEffects, ToneLibrary, WebCore, iLifeSlideshow |
| `_glTexSubImage2D` | gl core |  |  | QuartzCore | QuartzCore | QuartzCore, WebCore | QuartzCore, WebCore | CoreVideo, FaceCoreLight, WebCore |
| `_glTranslatef` | gl core | MapKit, QuartzCore | MapKit | MapKit, QuartzCore | MapKit, iLifeSlideshow | MapKit, iLifeSlideshow | MapKit, iLifeSlideshow | iLifeSlideshow |
| `_glUniform1f` | gl core |  | QuartzCore |  | QuartzCore | WebCore | PhotoBoothEffects, WebCore | GLKit, PhotoBoothEffects, WebCore |
| `_glUniform1fv` | gl core |  | QuartzCore |  | QuartzCore | QuartzCore, WebCore | QuartzCore, WebCore | WebCore |
| `_glUniform1i` | gl core |  | QuartzCore |  | QuartzCore | QuartzCore, WebCore | PhotoBoothEffects, QuartzCore, WebCore | GLKit, PhotoBoothEffects, ToneLibrary, WebCore |
| `_glUniform1iv` | gl core |  |  |  |  | WebCore | WebCore | WebCore |
| `_glUniform2f` | gl core |  |  |  |  | WebCore | PhotoBoothEffects, WebCore | PhotoBoothEffects, WebCore |
| `_glUniform2fv` | gl core |  | QuartzCore |  | QuartzCore | QuartzCore, WebCore | QuartzCore, WebCore | WebCore |
| `_glUniform2i` | gl core |  |  |  |  | WebCore | WebCore | WebCore |
| `_glUniform2iv` | gl core |  |  |  |  | WebCore | WebCore | WebCore |
| `_glUniform3f` | gl core |  |  |  |  | WebCore | WebCore | WebCore |
| `_glUniform3fv` | gl core |  | QuartzCore |  | QuartzCore | QuartzCore, WebCore | QuartzCore, WebCore | GLKit, WebCore |
| `_glUniform3i` | gl core |  |  |  |  | WebCore | WebCore | WebCore |
| `_glUniform3iv` | gl core |  |  |  |  | WebCore | WebCore | WebCore |
| `_glUniform4f` | gl core |  |  |  |  | WebCore | PhotoBoothEffects, WebCore | PhotoBoothEffects, WebCore |
| `_glUniform4fv` | gl core |  | QuartzCore |  | QuartzCore | QuartzCore, WebCore | QuartzCore, WebCore | GLKit, WebCore |
| `_glUniform4i` | gl core |  |  |  |  | WebCore | WebCore | WebCore |
| `_glUniform4iv` | gl core |  |  |  |  | WebCore | WebCore | WebCore |
| `_glUniformMatrix2fv` | gl core |  |  |  |  | WebCore | WebCore | WebCore |
| `_glUniformMatrix3fv` | gl core |  |  |  |  | WebCore | WebCore | GLKit, WebCore |
| `_glUniformMatrix4fv` | gl core |  | QuartzCore |  | QuartzCore | QuartzCore, WebCore | QuartzCore, WebCore | GLKit, ToneLibrary, WebCore |
| `_glUseProgram` | gl core |  | QuartzCore |  | QuartzCore | QuartzCore, WebCore | PhotoBoothEffects, QuartzCore, WebCore | GLKit, PhotoBoothEffects, ToneLibrary, WebCore |
| `_glValidateProgram` | gl core |  |  |  |  | WebCore | WebCore | GLKit, ToneLibrary, WebCore |
| `_glVertexAttrib1f` | gl core |  |  |  |  | WebCore | WebCore | WebCore |
| `_glVertexAttrib1fv` | gl core |  |  |  |  | WebCore | WebCore | WebCore |
| `_glVertexAttrib2f` | gl core |  |  |  |  | WebCore | WebCore | WebCore |
| `_glVertexAttrib2fv` | gl core |  |  |  |  | WebCore | WebCore | WebCore |
| `_glVertexAttrib3f` | gl core |  |  |  |  | WebCore | WebCore | WebCore |
| `_glVertexAttrib3fv` | gl core |  |  |  |  | WebCore | WebCore | WebCore |
| `_glVertexAttrib4f` | gl core |  |  |  |  | WebCore | WebCore | WebCore |
| `_glVertexAttrib4fv` | gl core |  |  |  |  | WebCore | WebCore | GLKit, WebCore |
| `_glVertexAttribPointer` | gl core |  | QuartzCore |  | QuartzCore | QuartzCore, WebCore | PhotoBoothEffects, QuartzCore, WebCore | GLKit, PhotoBoothEffects, ToneLibrary, WebCore |
| `_glVertexPointer` | gl core | MapKit, QuartzCore | MapKit, QuartzCore | MapKit, QuartzCore | MapKit, iLifeSlideshow | MapKit, iLifeSlideshow | MapKit, iLifeSlideshow | FaceCoreLight, iLifeSlideshow |
| `_glViewport` | gl core | MapKit, QuartzCore | MapKit, QuartzCore | MapKit, MediaToolbox, QuartzCore | MapKit, QuartzCore, iLifeSlideshow | MapKit, MediaToolbox, QuartzCore, WebCore, iLifeSlideshow | MapKit, MediaToolbox, PhotoBoothEffects, QuartzCore, WebCore, iLifeSlideshow | FaceCoreLight, GLKit, MediaToolbox, PhotoBoothEffects, ToneLibrary, WebCore, iLifeSlideshow |
| `_glBindFramebufferOES` | gl extension | MapKit | MapKit | MapKit, QuartzCore | MapKit, iLifeSlideshow | MapKit, iLifeSlideshow | MapKit, iLifeSlideshow | FaceCoreLight, iLifeSlideshow |
| `_glBindRenderbufferOES` | gl extension | MapKit | MapKit | MapKit | MapKit, iLifeSlideshow | MapKit, iLifeSlideshow | MapKit, iLifeSlideshow | FaceCoreLight, iLifeSlideshow |
| `_glBindVertexArrayOES` | gl extension |  |  |  |  |  |  | GLKit |
| `_glBlendEquationOES` | gl extension |  |  | QuartzCore |  |  |  |  |
| `_glCheckFramebufferStatusOES` | gl extension | MapKit | MapKit | MapKit | MapKit, iLifeSlideshow | MapKit, iLifeSlideshow | MapKit, iLifeSlideshow | FaceCoreLight, iLifeSlideshow |
| `_glDeleteFramebuffersOES` | gl extension | MapKit | MapKit | MapKit, QuartzCore | MapKit, iLifeSlideshow | MapKit, iLifeSlideshow | MapKit, iLifeSlideshow | iLifeSlideshow |
| `_glDeleteRenderbuffersOES` | gl extension | MapKit | MapKit | MapKit | MapKit, iLifeSlideshow | MapKit, iLifeSlideshow | MapKit, iLifeSlideshow | iLifeSlideshow |
| `_glDeleteVertexArraysOES` | gl extension |  |  |  |  |  |  | GLKit |
| `_glDiscardFramebufferEXT` | gl extension |  |  |  |  |  |  | GLKit |
| `_glFinishObjectAPPLE` | gl extension |  | QuartzCore | QuartzCore | QuartzCore | QuartzCore | QuartzCore | CoreVideo |
| `_glFinishTextureAPPLE` | gl extension | QuartzCore |  |  |  |  |  |  |
| `_glFramebufferParameteriAPPLE` | gl extension |  |  | QuartzCore | QuartzCore | QuartzCore | QuartzCore |  |
| `_glFramebufferRenderbufferOES` | gl extension | MapKit | MapKit | MapKit | MapKit, iLifeSlideshow | MapKit, iLifeSlideshow | MapKit, iLifeSlideshow | FaceCoreLight, iLifeSlideshow |
| `_glFramebufferTexture2DOES` | gl extension |  |  | QuartzCore |  |  |  |  |
| `_glGenFramebuffersOES` | gl extension | MapKit | MapKit | MapKit, QuartzCore | MapKit, iLifeSlideshow | MapKit, iLifeSlideshow | MapKit, iLifeSlideshow | FaceCoreLight, iLifeSlideshow |
| `_glGenRenderbuffersOES` | gl extension | MapKit | MapKit | MapKit | MapKit, iLifeSlideshow | MapKit, iLifeSlideshow | MapKit, iLifeSlideshow | FaceCoreLight, iLifeSlideshow |
| `_glGenVertexArraysOES` | gl extension |  |  |  |  |  |  | GLKit |
| `_glGenerateMipmapOES` | gl extension |  |  | QuartzCore |  |  |  |  |
| `_glGetRenderbufferParameterivOES` | gl extension | MapKit | MapKit | MapKit | MapKit, iLifeSlideshow | MapKit, iLifeSlideshow | MapKit, iLifeSlideshow | iLifeSlideshow |
| `_glLabelObjectEXT` | gl extension |  |  |  |  |  |  | GLKit |
| `_glMapBufferOES` | gl extension | MapKit | MapKit, QuartzCore | MapKit | MapKit, QuartzCore | MapKit, QuartzCore | MapKit, QuartzCore |  |
| `_glPopGroupMarkerEXT` | gl extension |  |  |  |  |  |  | GLKit |
| `_glPushGroupMarkerEXT` | gl extension |  |  |  |  |  |  | GLKit |
| `_glRenderbufferStorageMultisampleAPPLE` | gl extension |  |  |  |  | WebCore | WebCore | GLKit, WebCore |
| `_glRenderbufferStorageOES` | gl extension |  |  |  |  |  | MapKit | FaceCoreLight |
| `_glResolveMultisampleFramebufferAPPLE` | gl extension |  |  |  |  | WebCore | WebCore | GLKit, WebCore |
| `_glTestObjectAPPLE` | gl extension |  |  |  |  |  |  | CoreVideo |
| `_glTexImageCoreSurfaceAPPLE` | gl extension | QuartzCore |  |  |  |  |  |  |
| `_glUnmapBufferOES` | gl extension | MapKit | MapKit, QuartzCore | MapKit | MapKit, QuartzCore | MapKit, QuartzCore | MapKit, QuartzCore |  |
| `_EAGLMemoryNotificationRecycling` | private |  |  | QuartzCore |  | QuartzCore | QuartzCore | QuartzCore |
| `API` | EAGL selector (public) |  |  | QuartzCore | QuartzCore | QuartzCore | QuartzCore | CoreImage, QuartzCore |
| `APIs` | EAGL selector (private) |  |  |  |  |  |  | GLKit |
| `GetMacroContextPrivate` | EAGL selector (private) |  |  |  |  |  |  | CoreImage, QuartzCore |
| `attachImage:toCoreSurface:invertedRender:` | EAGL selector (private) | MapKit | MapKit, QuartzCore | MapKit, MediaToolbox, QuartzCore | MapKit, QuartzCore, iLifeSlideshow | MapKit, MediaToolbox, QuartzCore, iLifeSlideshow | MapKit, MediaToolbox, PhotoBoothEffects, QuartzCore, iLifeSlideshow | CoreImage, MediaToolbox, PhotoBoothEffects, QuartzCore, iLifeSlideshow |
| `currentContext` | EAGL selector (public) | MapKit | MapKit, QuartzCore | MapKit, MediaToolbox, QuartzCore | MapKit, QuartzCore, iLifeSlideshow | MapKit, MediaToolbox, QuartzCore, WebCore, iLifeSlideshow | MapKit, MediaToolbox, QuartzCore, WebCore, iLifeSlideshow | GLKit, MediaToolbox, PhotoBoothEffects, QuartzCore, ToneLibrary, WebCore, iLifeSlideshow |
| `getGLIShared` | EAGL selector (private) |  |  |  |  |  |  | OpenCL |
| `initWithAPI:` | EAGL selector (public) | MapKit | MapKit | MapKit, MediaToolbox | MapKit, iLifeSlideshow | MapKit, MediaToolbox, WebCore, iLifeSlideshow | MapKit, MediaToolbox, PhotoBoothEffects, WebCore, iLifeSlideshow | FaceCoreLight, MediaToolbox, ToneLibrary, WebCore, iLifeSlideshow |
| `initWithAPI:properties:` | EAGL selector (private) |  | QuartzCore | QuartzCore | QuartzCore | QuartzCore | QuartzCore | CoreImage, PhotoBoothEffects, QuartzCore |
| `initWithAPI:sharegroup:` | EAGL selector (public) | MapKit | MapKit | MapKit | MapKit, iLifeSlideshow | MapKit, iLifeSlideshow | MapKit, iLifeSlideshow | GLKit, iLifeSlideshow |
| `presentRenderbuffer:` | EAGL selector (public) | MapKit | MapKit | MapKit | MapKit, iLifeSlideshow | MapKit, WebCore, iLifeSlideshow | MapKit, WebCore, iLifeSlideshow | GLKit, ToneLibrary, WebCore, iLifeSlideshow |
| `renderbufferStorage:fromDrawable:` | EAGL selector (public) | MapKit | MapKit | MapKit | MapKit, iLifeSlideshow | MapKit, WebCore, iLifeSlideshow | MapKit, WebCore, iLifeSlideshow | GLKit, ToneLibrary, WebCore, iLifeSlideshow |
| `sendNotification:forTransaction:onLayer:` | EAGL selector (private) |  |  | QuartzCore | QuartzCore | QuartzCore | QuartzCore | QuartzCore |
| `setCurrentContext:` | EAGL selector (public) | MapKit | MapKit, QuartzCore | MapKit, MediaToolbox, QuartzCore | MapKit, QuartzCore, iLifeSlideshow | MapKit, MediaToolbox, QuartzCore, WebCore, iLifeSlideshow | MapKit, MediaToolbox, PhotoBoothEffects, QuartzCore, WebCore, iLifeSlideshow | FaceCoreLight, GLKit, MediaToolbox, PhotoBoothEffects, QuartzCore, ToneLibrary, WebCore, iLifeSlideshow |
| `sharegroup` | EAGL selector (public) | MapKit | MapKit | MapKit | MapKit, iLifeSlideshow | MapKit, iLifeSlideshow | MapKit, iLifeSlideshow | CoreImage, GLKit, PhotoBoothEffects, QuartzCore, iLifeSlideshow |
| `swapNotification:forTransaction:onLayer:` | EAGL selector (private) |  | QuartzCore | QuartzCore | QuartzCore | QuartzCore | QuartzCore | QuartzCore |
| `texImageIOSurface:target:internalFormat:width:height:format:type:plane:invert:` | EAGL selector (private) |  |  |  |  |  |  | CoreImage, CoreVideo, QuartzCore |

## OpenGLES exports by kind

| kind | 5H11a | 7E18 | n72-8C148 | 7B500 | 8C148 | 8L1 | 9B206 |
|---|---|---|---|---|---|---|---|
| EAGL class | 4 | 4 | 4 | 4 | 4 | 4 | 4 |
| EAGL constant | 4 | 6 | 6 | 6 | 6 | 6 | 7 |
| EAGL function | 1 | 1 | 1 | 1 | 1 | 1 | 1 |
| EGL | 30 | 0 | 0 | 0 | 0 | 0 | 0 |
| gl core | 144 | 247 | 247 | 247 | 247 | 247 | 239 |
| gl extension | 34 | 42 | 50 | 43 | 50 | 50 | 94 |
| private | 1 | 9 | 10 | 9 | 10 | 10 | 12 |

### Private and EAGL exports, per build

| symbol | kind | 5H11a | 7E18 | n72-8C148 | 7B500 | 8C148 | 8L1 | 9B206 |
|---|---|---|---|---|---|---|---|---|
| `_EAGLGetCurrentMacroContextPrivate` | private |  |  |  |  |  |  | x |
| `_EAGLGetVersion` | EAGL function | x | x | x | x | x | x | x |
| `_EAGLMemoryNotificationRecycling` | private |  |  | x |  | x | x | x |
| `_GLCBackDispatch` | private |  | x | x | x | x | x | x |
| `_GLCFrontDispatch` | private |  | x | x | x | x | x | x |
| `_GLCGetProfilerStorage` | private |  | x | x | x | x | x | x |
| `_GLCRestoreDispatch` | private |  | x | x | x | x | x | x |
| `_GLCRestoreDispatchFunction` | private |  | x | x | x | x | x | x |
| `_GLCSelectDispatchBounded` | private |  | x | x | x | x | x | x |
| `_GLCSelectDispatchFunction` | private |  | x | x | x | x | x | x |
| `_GLCSetProfilerStorage` | private |  | x | x | x | x | x | x |
| `_GLIContextFromEAGLContext` | private |  | x | x | x | x | x | x |
| `_OBJC_CLASS_$_EAGLContext` | EAGL class | x | x | x | x | x | x | x |
| `_OBJC_CLASS_$_EAGLSharegroup` | EAGL class | x | x | x | x | x | x | x |
| `_OBJC_IVAR_$_EAGLContext._private` | private |  |  |  |  |  |  | x |
| `_OBJC_METACLASS_$_EAGLContext` | EAGL class | x | x | x | x | x | x | x |
| `_OBJC_METACLASS_$_EAGLSharegroup` | EAGL class | x | x | x | x | x | x | x |
| `_eglBindTexImage` | EGL | x |  |  |  |  |  |  |
| `_eglChooseConfig` | EGL | x |  |  |  |  |  |  |
| `_eglCopyBuffers` | EGL | x |  |  |  |  |  |  |
| `_eglCreateContext` | EGL | x |  |  |  |  |  |  |
| `_eglCreatePbufferSurface` | EGL | x |  |  |  |  |  |  |
| `_eglCreatePixmapSurface` | EGL | x |  |  |  |  |  |  |
| `_eglCreateWindowSurface` | EGL | x |  |  |  |  |  |  |
| `_eglDestroyContext` | EGL | x |  |  |  |  |  |  |
| `_eglDestroySurface` | EGL | x |  |  |  |  |  |  |
| `_eglEnableInternalSurface` | EGL | x |  |  |  |  |  |  |
| `_eglGetConfigAttrib` | EGL | x |  |  |  |  |  |  |
| `_eglGetConfigs` | EGL | x |  |  |  |  |  |  |
| `_eglGetCurrentContext` | EGL | x |  |  |  |  |  |  |
| `_eglGetCurrentDisplay` | EGL | x |  |  |  |  |  |  |
| `_eglGetCurrentSurface` | EGL | x |  |  |  |  |  |  |
| `_eglGetDisplay` | EGL | x |  |  |  |  |  |  |
| `_eglGetError` | EGL | x |  |  |  |  |  |  |
| `_eglGetProcAddress` | EGL | x |  |  |  |  |  |  |
| `_eglInitialize` | EGL | x |  |  |  |  |  |  |
| `_eglMakeCurrent` | EGL | x |  |  |  |  |  |  |
| `_eglQueryContext` | EGL | x |  |  |  |  |  |  |
| `_eglQueryString` | EGL | x |  |  |  |  |  |  |
| `_eglQuerySurface` | EGL | x |  |  |  |  |  |  |
| `_eglReleaseTexImage` | EGL | x |  |  |  |  |  |  |
| `_eglSurfaceAttrib` | EGL | x |  |  |  |  |  |  |
| `_eglSwapBuffers` | EGL | x |  |  |  |  |  |  |
| `_eglSwapInterval` | EGL | x |  |  |  |  |  |  |
| `_eglTerminate` | EGL | x |  |  |  |  |  |  |
| `_eglWaitGL` | EGL | x |  |  |  |  |  |  |
| `_eglWaitNative` | EGL | x |  |  |  |  |  |  |
| `_kEAGLColorFormatRGB565` | EAGL constant | x | x | x | x | x | x | x |
| `_kEAGLColorFormatRGBA8` | EAGL constant | x | x | x | x | x | x | x |
| `_kEAGLContextPropertyAccelerated` | EAGL constant |  | x | x | x | x | x | x |
| `_kEAGLContextPropertyClientRetainRelease` | EAGL constant |  |  |  |  |  |  | x |
| `_kEAGLContextPropertySharegroup` | EAGL constant |  | x | x | x | x | x | x |
| `_kEAGLDrawablePropertyColorFormat` | EAGL constant | x | x | x | x | x | x | x |
| `_kEAGLDrawablePropertyRetainedBacking` | EAGL constant | x | x | x | x | x | x | x |
| `_opengl_error_break` | private | x |  |  |  |  |  |  |

## EAGL classes' methods (OpenGLES's own ObjC metadata)

| class | method | 5H11a | 7E18 | n72-8C148 | 7B500 | 8C148 | 8L1 | 9B206 |
|---|---|---|---|---|---|---|---|---|
| EAGLContext | `+currentContext` | x | x | x | x | x | x | x |
| EAGLContext | `+setCurrentContext:` | x | x | x | x | x | x | x |
| EAGLContext | `-API` | x | x | x | x | x | x | x |
| EAGLContext | `-GetMacroContextPrivate` |  |  |  |  |  |  | x |
| EAGLContext | `-attachImage:toCoreSurface:invertedRender:` | x | x | x | x | x | x | x |
| EAGLContext | `-dealloc` | x | x | x | x | x | x | x |
| EAGLContext | `-getParameter:to:` |  | x | x | x | x | x | x |
| EAGLContext | `-initWithAPI:` | x | x | x | x | x | x | x |
| EAGLContext | `-initWithAPI:properties:` |  | x | x | x | x | x | x |
| EAGLContext | `-initWithAPI:sharedWithCompute:` |  |  |  |  |  |  | x |
| EAGLContext | `-initWithAPI:sharegroup:` | x | x | x | x | x | x | x |
| EAGLContext | `-presentRenderbuffer:` | x | x | x | x | x | x | x |
| EAGLContext | `-renderbufferStorage:fromDrawable:` | x | x | x | x | x | x | x |
| EAGLContext | `-sendNotification:forTransaction:onLayer:` |  |  | x | x | x | x | x |
| EAGLContext | `-setParameter:to:` |  | x | x | x | x | x | x |
| EAGLContext | `-sharegroup` | x | x | x | x | x | x | x |
| EAGLContext | `-swapNotification:forTransaction:onLayer:` | x | x | x | x | x | x | x |
| EAGLContext | `-texImageIOSurface:target:internalFormat:width:height:format:type:plane:invert:` |  |  | x |  | x | x | x |
| EAGLSharegroup | `-APIs` |  | x | x | x | x | x | x |
| EAGLSharegroup | `-dealloc` | x | x | x | x | x | x | x |
| EAGLSharegroup | `-getGLIShared` |  |  |  |  |  |  | x |
| EAGLSharegroup | `-init` | x |  | x |  | x | x | x |
| EAGLSharegroup | `-initWithAPI:` |  |  | x |  | x | x | x |
| EAGLSharegroup | `-initWithAPI:require_acceleration:` |  | x |  | x |  |  |  |
| EAGLSharegroup | `-initWithAPI:sharedWithCompute:` |  |  |  |  |  |  | x |
| EAGLSharegroup | `-loadGLIPlugin:` |  | x | x | x | x | x |  |
| EAGLSharegroup | `-loadGLIPlugin:sharedWithCompute:` |  |  |  |  |  |  | x |

## Selectors OpenGLES sends to other objects

| selector | 5H11a | 7E18 | n72-8C148 | 7B500 | 8C148 | 8L1 | 9B206 |
|---|---|---|---|---|---|---|---|
| `boolValue` | x | x | x | x | x | x | x |
| `drawableProperties` | x | x | x | x | x | x | x |
| `isKindOfClass:` | x | x | x | x | x | x | x |
| `nativeWindow` | x | x | x | x | x | x | x |
| `numberWithBool:` |  |  |  |  |  |  | x |
| `objectForKey:` | x | x | x | x | x | x | x |
| `performSelector:` | x | x | x | x | x | x | x |
| `respondsToSelector:` | x | x | x | x | x | x | x |
| `setObject:forKey:` |  | x | x | x | x | x | x |
| `unsignedIntegerValue` |  | x | x | x | x | x | x |

## Private structures (@encode)

| structure | 5H11a | 7E18 | n72-8C148 | 7B500 | 8C148 | 8L1 | 9B206 |
|---|---|---|---|---|---|---|---|
| `_EAGLNativeWindowObject` | (unnamed) i^?^?^?^?^? | version, attach, detach, begin, swap, collect | version, attach, detach, begin, swap, collect, properties | version, attach, detach, begin, swap, collect | version, attach, detach, begin, swap, collect, properties | version, attach, detach, begin, swap, collect, properties | (unnamed) i^?^?^?^?^?^? |
| `EAGLNativeWindowCallbacksRec` | absent | callback_data, create_buffer, destroy_buffer | absent | callback_data, create_buffer, destroy_buffer | absent | absent | absent |
| `__GLIContextRec` | absent | absent | absent | absent | absent | absent | absent |
| `_EAGLIOSurface` | absent | absent | absent | absent | absent | absent | absent |
| `__GLIFunctionDispatchRec` fields | absent | 822 | 841 | 826 | 841 | 870 | 905 |
