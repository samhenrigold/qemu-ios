> Status: research, superseded by `../../contrib/ipad1-gles/README.md` (the GLI shim as built).

# iPad1,1 iOS 3.2.2 (7B500): userland GL and display paths, resolved by symbol

Scope: the four questions left open by `gap-display-stack-kernel.md` §3–§5 and `web-sgx.md` §1.4/§2.
All code was read from the UUID-matched SDK binaries (`x-iPhoneSDK3_2_2/.../iPhoneOS3.2.sdk`, armv7 slices).
**Cache VA = image base + SDK offset.** I checked this against `dsc-symbols.tsv` at several points, e.g.
`EAGLServer::renderer` 0x76844 → 0x342ef844 and `loadGLIPlugin:` 0x4a70 → 0x336aca70.

| image | 7B500 cache base | notes |
|---|---|---|
| OpenGLES | 0x336a8000 | EAGL, loader, 290 gl* trampolines |
| QuartzCore | 0x34279000 | same code offsets as 7B367 (the 7B367 addresses 0x309c7844/0x309c9594/0x309c7660 = SDK 0x76844/0x78594/0x76660) |
| UIKit | 0x3223e000 | |
| GraphicsServices | 0x3414b000 | |
| libGFXShared.dylib | 0x3098b000 | |
| GLEngine.bundle, IMGSGX535GLDriver.bundle | not in cache | standalone files on the rootfs. Addresses below are file VAs |
| MBXGLEngine.bundle (3.2.2) | not on the iPad rootfs | only in the SDK. I used it as the reference implementation of the 3.2 MBX ABI |

Working files: `7B500/work/gl/` holds the thin slices, the `*.od.ann` annotated disassembly, the `ann.py` literal/stub
resolver, and the scripts that produced the TSV.

---

## 1. GPU-less compositing in QuartzCore

### 1.1 Which server the iPad gets
- `-[CAWindowServer _detectDisplays]` (0x2014 / 0x3427b014) walks a table of display `open()` functions.
  On the iPad the one that matches is `H3CLCDDisplay::open()` (0x771a4), which uses `IOServiceMatching("AppleCLCD")`
  and builds an `H3Display` (0x76c84). The table also has `M2CLCDDisplay::open` for "AppleM2CLCD" and TV-out variants.
- `H3Display::new_server()` (0x77110 / 0x342f0110) **always** builds
  `EAGLServer(display, "LCD", api=2)`, so the iPad compositor asks for **ES2**. There is no plain-`Server` branch here.
  `M2Display::new_server` does have one: it uses EAGLServer only when +0x168==2.
- `EAGLServer::EAGLServer` (0x76930) latches `enable_ogl`, which is on by default. `CA_ENABLE_OGL` is read once and only
  `CA_ENABLE_OGL=0` turns it off.

### 1.2 How CA ends up on the software renderer
- `EAGLServer::render_update` (0x768c0 / 0x342ef8c0) calls `EAGLServer::renderer()` (0x76844):
  - If `this+0xfc` (the renderer) is NULL **and** `enable_ogl` is set, it calls
    `ctx = Display::eagl_context(this+0x100 = 2)` (0x23630). That calls `_CAEAGLContextCreate(2, nil)` (0x23658), which
    runs `[[EAGLContext alloc] initWithAPI:2 properties:{kEAGLContextPropertyAccelerated: YES}]`.
  - If `ctx != nil`, the renderer is `CARenderOGLNew(&kCARenderGLESCallbacks, {2, ctx})`.
  - If `ctx == nil` or `enable_ogl == 0`, it returns NULL.
- When `renderer()` returns NULL, `EAGLServer::render_update` tail-calls **`Server::render_update` (0x78f8c / 0x342f1f8c)**.
  That calls `Server::sw_renderer()` (0x78f40), which does
  `CARenderOGLNew(&kCARenderSoftwareCallbacks, implicit_renderer_flags|flags)` and caches the result at `this+0x84`.
  It then calls `_CARenderOGLRenderDisplay`.
  `EAGLServer::render_surface` (0x769b0) falls back to `Server::render_surface` (0x78fa4) in the same way.
- The software renderer is `CA::OGL::SWContext` (0x6853c…). `SWContext::make_buffer_current` (0x68b50) locks the display's
  page `Surface` and renders into its pixels directly (`set_destination`). It accepts formats 'BGRA', 'ARGB' and 565.
  **It needs no GPU and no scaler.**
- Because CA passes `Accelerated=YES`, the context needs an *accelerated* GLI renderer or the MBX engine, and the MBX
  engine is ES1-only (see §2). So CA's compositor never runs on an ES1 MBX shim. It runs either on an accelerated ES2 GLI
  engine or in software.

### 1.3 What happens when `IOSurfaceAcceleratorCreate` fails (the step the earlier study couldn't trace)
- `Display::iosurface_accelerator()` (0x76660 / 0x342ef660) calls `IOSurfaceAcceleratorCreate(NULL, 0, &this->0x48)` and
  returns the pointer. On failure it returns NULL, and because NULL is not cached it tries again on every call.
- Its callers:
  1. `CA::WindowServer::IOSurface::copy_iosurface` (0x78594 / 0x342f1594; this is the study's 0x309c9594). It is a
     `Surface` **virtual (vtable +0x20)** and returns 0 when there is no accelerator.
     Its only compositor caller is **`IOMFBDisplay::finish_update` (0x10fcc)**, at the indirect call 0x113f8 / 0x3428a3f8.
     That call copies the previously displayed page into the new page, rotating or flipping it through the scaler.
     **finish_update ignores the return value**: there is no CPU fallback, and the copy just doesn't happen.
     The copy only runs for page surfaces with flag bit 1 set in `+0x18`. `update_surface` (0x23a10) sets that bit when
     display+0x5f bit 0 is set, which `update_actual_bounds` sets when logical bounds ≠ display bounds.
     *Unverified: how often that is true at runtime for the full-screen LCD.* The safe choice is
     **`MBX2D_PAGE_FLIP=0`** (or `MBX2D_OFFSCREEN=0`). The `IOMFBDisplay` ctor at 0x2270–0x22a0 then sets
     `page_flipping_disabled`, which leaves one page and nothing to copy.
  2. `GLESContext::bind_image_impl` (0x2a3ae) handles YUV→BGRA conversion on the GL path only. With no accelerator it
     skips the conversion.
  3. `EAGLServer::add_iosurface_accelerator_token` (0x76a60) is on the GL path only.
- `Server::render_display` (0x78fd0) serves `CARenderServerRenderDisplay` and UIKit screenshots. It re-renders the tree
  into the target IOSurface (`render_surface`) and uses no accelerator.

### 1.4 Who rotates the portrait UI on the 1024×768 panel
**UIKit and GraphicsServices do it in the layer tree, not CA's compositor and not the scaler.**
- `IOMFBDisplay::update_framebuffer` (0x77600) sets the display bounds straight from
  `IOMobileFramebufferGetDisplaySize` (0x776a2), with no swap. The CA display is therefore 1024×768.
- `+[UIApplication _startWindowServerIfNecessary]` (UIKit 0x12fc / 0x3223f2fc) runs in SpringBoard, which renders locally.
  It does `[[CAWindowServer server] setRendererFlags:3]` and reads `displays[0].bounds`. If `h < w` it swaps them and calls
  `GSSetMainScreenInfo(768, 1024, 1.0, orientation = π/2)`; the literal is 0x3fc90fdb.
- `GSSetMainScreenInfo` (GS 0x28f4 / 0x3414d8f4) calls `_ResetTransforms` (0x2794). That builds
  `__screenWindowTransform = CGAffineTransformMakeRotation(-orientation)`, plus a position transform, and UIKit applies it
  to windows through `GSMainScreenWindowTransform`. Rotation is therefore an ordinary layer transform, which SWContext
  renders like any other.
- `Display::set_logical_orientation` (0xb3e0) adds 90° when the display is wider than it is tall. The resulting byte (+0x5e)
  is read only by `GLESContext::make_buffer_current` (0x24238, as a tiling hint) and by `-[CAWindowServerDisplay orientation]`.
  It is **not** applied as a scene transform.
- `IOMFBDisplay::display_supports_rotation_p` (0x77228) returns true only for rotation 0. The pipe itself is never asked
  to rotate.
- So a display with no GPU and no scaler, a 1024×768 panel and a portrait UI works, provided the IOMFB model reports
  1024×768. That rests on the openiBoot k48 table, as before, and is unverified on hardware.

### 1.5 Dependence on GL
- SpringBoard (3.2.2 SDK) links IOSurface, IOMobileFramebuffer, CoreSurface and QuartzCore. It has **no OpenGLES, EAGL or gl\*
  imports**. UIKit **does not link OpenGLES** either. This confirms the earlier finding.
- **Cost of retrying (new):** OpenGLES `_eagl_init` (0x4f9c / 0x336acf9c) does **not cache failure**.
  `_eagl_initialized` is set only on success, at 0x521e. `Display::eagl_context` doesn't cache nil either. Every attempt
  therefore repeats the CFBundle lookup, `dlopen(GLEngine)`, IOAcceleratorES matching, `gliInitializeLibrary`,
  `gliGetVersion`, `dlclose`, and the AppleMBXDevice match.
  - `renderer()` retries on each render when `enable_ogl` is set, so set **`CA_ENABLE_OGL=0`** in SpringBoard's launchd
    `EnvironmentVariables`.
  - `EAGLServer::add_swap_token` (0x76a90), `add_iosurface_accelerator_token` (0x76a60) and `flush_command_stream`
    (0x76a20) call `eagl_context` **without** checking `enable_ogl`. They are reached from `IOMFBDisplay::finish_update`
    through display+0x160, the server's secondary vtable. *How often, is unverified.* Once any GL shim below is installed,
    `_eagl_init` succeeds once and these retries become cheap.
- If the `sgx` DT node stays and IMGSGX535.kext registers `IOAcceleratorES`, `_eagl_init` goes down the real GLI and SGX
  path: GLEngine → libGFXShared → `/System/Library/Extensions/IMGSGX535GLDriver.bundle` → IOKit calls to a dead GPU.
  Keep removing that node, or replace GLEngine (see §4).

---

## 2. The MBX fallback in OpenGLES 3.2.2, and whether mbxshim runs unmodified

### 2.1 Sequence
1. `-[EAGLContext initWithAPI:properties:]` (0x3fb0 / 0x336abfb0):
   - Reads `EAGLContextPropertySharegroup` and `EAGLContextPropertyAccelerated`; the default for Accelerated is NO.
   - Creates `[[EAGLSharegroup alloc] initWithAPI:api require_acceleration:acc]` unless a sharegroup was passed.
   - Callocs the context private block **X = 0x19E0 bytes** with this layout:
     `+0 EAGLContext*, +4 sharegroup, +8 api, +0xC GC, +0x10 front dispatch (0xCE8), +0xCF8 back dispatch (0xCE8)`.
2. `-[EAGLSharegroup initWithAPI:require_acceleration:]` (0x4b3c / 0x336acb3c):
   - Callocs the sharegroup private block `S = 0x1A18` with layout
     `+0 api, +4 engine (1=GLI, 2=MBX), +8 byte, +0xC/+0xCF4 dispatch pair, +0x19DC pixel-format copy (0x34), +0x1A10 GLI share ctx, +0x1A14 MBX sharegroup`.
   - Calls `_eagl_init()`.
   - Walks the GLI pixel-format list (`_gli_pixelformat`) for nodes with `flags(+8) & 0x100` (accelerated) and sends
     `loadGLIPlugin:` for each until `S+4 != 0`.
   - **If api == 1** and no engine was picked and `_mbx_dispatch != NULL`, it calls `mbx[+0](&S[0x1A14])`
     (`GLESCreateSharegroup`); nonzero means success and sets `S+4 = 2`.
   - If `!require_acceleration`, it walks the list again for the *non*-accelerated nodes.
   - **The MBX path is ES1-only on 3.2.**
3. `_eagl_init` (0x4f9c):
   - pthread_once, then locks the mutex and bumps the refcount.
   - GLI: `dlopen(GLEngine.bundle/GLEngine)`, then `dlsym("gliInitializeLibrary")`, then `IOServiceGetMatchingService("IOAcceleratorES")`.
     It calls `gliInitializeLibrary(&accel,0,1,cb,0,0,0)` if the service exists, or `(0,0,0,cb,0,0,0)` if not.
     It then dlsyms 17 names from `_eagl_gli_names` (0x15bf4, 32-byte stride, stored at `_gli_plugin+4…+0x44`), calls
     `gliGetVersion(&a,&b,&c)` (nonzero means OK) and `gliChoosePixelFormat(&_gli_pixelformat, attribs)`.
   - **Whether or not GLI succeeded, it then tries MBX:**
     `IOMasterPort` → `IOServiceGetMatchingServices(IOServiceMatching("AppleMBXDevice"))`, which needs at least one match →
     `CFBundleCopyResourceURL(com.apple.opengles, "MBXGLEngine","bundle")` → `CFBundleCreate` →
     **`CFBundleGetDataPointerForName("GLESGetEGLInterface")`** → call it → `_mbx_dispatch = result`.
   - Both engines can be live at the same time.
4. `-[EAGLSharegroup loadGLIPlugin:pf]` (0x4a70 / 0x336aca70):
   - Copies 13 words of the pixel-format node to `S+0x19DC` and clears its next pointer.
   - Calls `gliCreateContext(&S[0x1A10], &S[0x19DC], NULL, &S[0xC], &S[0xCF4], api==1?4 : api==2?8:0)`.
     Zero means success and sets `S+4 = 1`.
5. The context engine call in `initWithAPI:properties:`:
   - GLI: `gliCreateContext(&X[0xC], &S[0x19DC], S[0x1A10], &X[0x10], &X[0xCF8], 4|8)`. Zero is OK.
     For apps not linked on or after 3.2 (`_CFExecutableLinkedOnOrAfter(0x3e9)` false) it adds
     `gliSetInteger(gc, 0x3e3, {1})`.
   - MBX: **`GLESCreateGC(S[0x1A14], X+0x10, X+0xCF8, X+0xC)`**. Nonzero is success; 0 means release and return nil.

### 2.2 Field-by-field against mbxshim (3.1.3)

| item | 3.1.3 (mbxshim) | 3.2.2 (OpenGLES + real 3.2.2 MBXGLEngine) | same? |
|---|---|---|---|
| discovery | `AppleMBXDevice` match, then `GLESGetEGLInterface` | same; `CFBundleGetDataPointerForName` | yes |
| interface table | 9 fn ptrs + ext pairs | 9 fn ptrs, same order and meaning (every OpenGLES call site checked: +0 CreateSharegroup(void\*\*) returns ≠0 OK; +4 DestroySharegroup; +8 CreateGC; +0xC DestroyGC; +0x10 BindCoreSurface(gc, target, IOSurface); +0x14 BindView(gc, `[layer nativeWindow]`, ifmt 0x8058/0x8d62, retained); +0x1C PresentView(gc); +0x20 SwapNotification(gc, `IOMobileFramebufferGetID()` out, transaction, layer)). +0x18 FinishTexture is not called by OpenGLES 3.2.2 | yes |
| GLESCreateGC args | (sg, X+0x10, **X+0xCE8**, X+0xC) | (sg, X+0x10, **X+0xCF8**, X+0xC) | arg 3 moved. It is the *back* dispatch table, `X+0x10+sizeof(dispatch)`, and it moved because the table grew. mbxshim ignores arg 3, so no effect |
| GLESCreateGC return | 1 = OK | nonzero = OK. The real 3.2.2 engine (`_GLESCreateGC` 0x7ac0) callocs a 0x1530 GC, keeps table/back at GC+0x1528/+0x152C and stores the GC through arg 4 | yes |
| gl\* trampoline | TSD +0xC0, GC at ctx+0xC, fn at ctx+0x10+4·slot | same offsets; now thumb with a `cbz` null-ctx check | yes |
| **dispatch size** | **822 slots (0xCD8)** | **826 slots (0xCE8)**. `_GLCSelectDispatchBounded` clamps to 0x33A; the real 3.2.2 `_SetDispatch` loops to 0xCE8 | **no** |
| **slot numbering** | as in `slotmap.txt` | slots 0–760 unchanged; **3 slots inserted at 761–763** (`vertex_attrib_divisor`, `draw_arrays_instanced`, `draw_elements_instanced`); **every 3.1.3 slot ≥761 is +3**; **825 new** (`glFramebufferParameteriAPPLE`). 441–447 were renamed from NV combiners to UBO entries, which ES never uses | **no** |
| CA drawable ABI | vt[1] bind(drawable,fourcc,block), vt[2] unbind, vt[4] present(drawable,1); block {ctx, CreateBuffer(ctx,IOSurface), DestroyBuffer} | identical. The real 3.2.2 `_GLESBindView` 0xa8d8 stores at +0x20c/+0x210/+0x214/+0x218/+0x224 and calls `[r5,#4]`; `_GLESPresentView` 0xa864 calls `[drawable,#0x10]` with r1=1 | yes |
| API coverage | ES1 | **ES1 only**: api==2 never reaches MBX | — |

Proof of the +3 shift, from three independent sources that agree:
- The 3.2 trampolines: 290 of them, and 62 differ from slotmap.txt, all by exactly +3 from 761 up.
- The real 3.2.2 MBX `_SetDispatch` stores `alpha_funcx` at slot 764.
- The ObjC `@encode` diff between the 3.1.3 and 3.2 OpenGLES binaries.

### 2.3 Verdict: mbxshim does **not** work unmodified. The exact changes:
1. `genstubs.py`: `N_SLOTS = (0xCF8 - 0x10) // 4` (= 826). Regenerate `slotmap.txt` from the **3.2** OpenGLES (armv7).
   The regex still matches `ldr r3,[r3,#off]`, and `TSD_OFF=0xc0` still holds.
2. `GLESCreateGC` in `mbxshim.c`: add 3 to every hard-coded `table[N]` with N ≥ 761:
   761→764 alphaFuncx, 762→765 clearColorx, 763→766 clearDepthf, 764→767 clearDepthx, 767→770 color4x,
   768→771 depthRangef, 769→772 depthRangex, 772→775 frustumf, 773→776 frustumx, 785→788 lineWidthx,
   786→789 loadMatrixx, 789→792 multMatrixx, 790→793 normal3x, 791→794 orthof, 792→795 orthox, 793→796 pointSizex,
   794→797 polygonOffsetx, 795→798 rotatex, 796→799 scalex, 799→802 texParameterx, 801→804 translatex,
   802→805 multiTexCoord4x, 803→806 sampleCoveragex.
   Slots <761 (everything else mbxshim fills) are unchanged.
3. Keep the **wire** slot numbers in `qc(...)`, i.e. the 3.1.3 IDs such as `qc(791,…)` for orthof. The host decoder then
   stays unchanged; only the guest table indices move.
4. Install `MBXGLEngine.bundle` (with an Info.plist naming the executable) under `/System/Library/Frameworks/OpenGLES.framework/`.
   The 3.2 rootfs has no such bundle.
5. Make `IOServiceGetMatchingServices(IOServiceMatching("AppleMBXDevice"))` match something. Either patch the 15-byte
   string at **0x336bdb5c**, or register a matching IOService.
6. Accept the limits: ES2 contexts return nil, and CA stays in software because it asks for ES2. The iPad needs the same
   guest-services `mcr p15,3,…,c15,c15,0` hook that mbxshim's `qc()` uses.

---

## 3. The GLI engine ABI (the ES2 path)

### 3.1 The 19 entry points
`_gli_plugin` = `{void *dlhandle; fn[17]}`. The offsets below are within that struct; gliInitializeLibrary and
gliTerminateLibrary are dlsym'd separately. Signatures come from the OpenGLES call sites and the GLEngine prologues.
Errors are CGLError-style: 0 = OK, 10014 (0x271e) = bad address, 10015 (0x271f) = no plugin.

| # | name (+off) | signature (as used) | called by OpenGLES 3.2.2? |
|---|---|---|---|
| – | gliInitializeLibrary | `void (const io_service_t *svcs, uint32 z, uint32 nsvcs, int (*flush_cb)(void), void *unused, void *io_data, void *lib_init)` → `gfxInitializeLibrary(svcs, z, nsvcs, cb, gliGetNewIOSurfaceES, io_data, lib_init)` | yes, from `_eagl_init` |
| – | gliTerminateLibrary | `void (void)` | yes, from `_eagl_dealloc` |
| 1 (+4) | gliChoosePixelFormat | `CGLError (GLIPixelFormat **out, const GLint *attribs)`. The pixel format is a linked list: +0 next, +4 renderer id (\|0x20000), +8 flags (**0x100 = accelerated**) … ≥0x34 bytes | yes |
| 2 (+8) | gliDestroyPixelFormat | `void (GLIPixelFormat *)` | yes, from dealloc |
| 3 (+0xC) | gliQueryRendererInfo | *unverified* (CGL analog `(mask, GLIRendererInfo*, GLint*)`) | no, but must exist |
| 4 (+0x10) | gliDestroyRendererInfo | *unverified* | no |
| 5 (+0x14) | gliCreateContext | `CGLError (GLIContext *out, GLIPixelFormat *pf, GLIContext share, GLIFunctionDispatch *front, GLIFunctionDispatch *back, GLuint api_bits /*4 ES1, 8 ES2*/)`. The engine keeps front/back at GC+0x54E0/+0x54E4 | yes |
| 6 (+0x18) | gliDestroyContext | `CGLError (GLIContext)` | yes |
| 7 (+0x1C) | gliAttachDrawable | *unverified* | no |
| 8 (+0x20) | gliAttachDrawableWithOptions | *unverified* | no |
| 9 (+0x24) | gliSwapBuffers | *unverified* | no |
| 10 (+0x28) | gliSetInteger | `CGLError (GLIContext, GLenum pname, const GLint *v)`. Private pnames: **0x2C1** swap notify {fbID, txn, layer}; **0x38E** attach IOSurface image {IOSurfaceID, target, ifmt, w, h, fmt, type, 0}; **0x399** {target, invert}; **0x39B** detach {IOSurfaceID, 0x8D41}; **0x3E3** {1} legacy-app flag; **0x7AA** profiler. Also every `-[EAGLContext setParameter:to:]` | yes |
| 11 (+0x2C) | gliGetInteger | `CGLError (GLIContext, GLenum, GLint *)` | yes |
| 12–14 (+0x30/+0x34/+0x38) | gliGetAttribute / gliSetAttribute / gliCopyAttributes | *unverified* | no |
| 15 (+0x3C) | gliGetVersion | `GLboolean (GLint*, GLint*, GLint*)`. Writes 2, 3, 0x20000 and returns 1 iff at least one gld plugin connected | yes |
| 16 (+0x40) | gliBindViewES | `void (GLIContext, void *nativeWindow_or_NULL, GLboolean retained, int, int)`. Stored on the bound framebuffer (+0x40, +0x280); GL_INVALID_OPERATION if none | yes |
| 17 (+0x44) | gliPresentViewES | `GLboolean (GLIContext)` | yes |

In the GLI path, EAGL does the CoreAnimation binding itself. In `renderbufferStorage:fromDrawable:` (0x43c0) it runs:
1. `gliBindViewES(gc,0,…)`
2. `drawable->vt[1](drawable, 'BGRA' | 'L565', {gc, _eagl_create_buffer (returns 1), _eagl_destroy_buffer (→ SetInteger 0x39B)})`
3. `surf = drawable->vt[3](drawable)`
4. `attachImage:GL_RENDERBUFFER toCoreSurface:surf` (→ SetInteger 0x38E)
5. `gliBindViewES(gc, drawable, retained, 0, 0)`

After that, GLEngine's `gliGetNewIOSurfaceES` (0xe6d30) pulls each new surface from `drawable->vt[3]`, caching up to 4.

### 3.2 `__GLIFunctionDispatchRec` and the TSV
- Authoritative source: the `@encode` of `_EAGLContextPrivate` in OpenGLES. It contains **two** `__GLIFunctionDispatchRec`
  of **826** named fields each (front and back). **1652 is the total of both tables, not the size of one.**
- **The fill code:** GLEngine `_gliInitDispatchTable` (0x74d0). It first sets all 826 slots to `_gliUnimplemented`
  (0xe4fd4), keeping a slot's saved value if the back table has one. If GC+0x683C==1 (ES1) it stores 187 handlers, 0x7512…0x8b48.
  If ==2 it calls `_gliSetDispatchOpenGLES2` (0xe4fdc), which stores 152. Then 6 common handlers follow:
  PushClientAttrib, PopClientAttrib, TextureRangeAPPLE, GetTexImage, GetTexLevelParameteriv, GetCompressedTexImage.
  All 345 stores land on the field whose name matches their `_glXxx_Exec` symbol. The only exception is the ARB/OES
  alias at 813.
- Slots 0–763 follow the current macOS `gliDispatch.h` order exactly. 764–825 are the ES tail.
- The table is **`~/Developer/qemu-ios-files/ipad1/7B500/gli-dispatch-7B500.tsv`**. Columns: slot, byte offset,
  EAGL-ctx offset, field, GL name, ES1-filled, ES2-filled, exported trampoline, 3.1.3 slot, and the macOS prototype.
  The GL name is the trampoline name, else the `_Exec` name, else derived from the field; the 554 derived ones are marked `*`.
- 15 exported trampolines hit slots GLEngine never fills, so they reach `gliUnimplemented`: GetTexLevelParameterfv,
  the CompressedTex\*3D pair, DrawRangeElements, **DeleteShader (591)**, the eight Query functions, GetBufferSubData
  and FramebufferTexture3D. *DeleteShader looks odd. It may be filled elsewhere; unverified.*

---

## 4. The GLEngine↔driver boundary, and where to forward ES2

- **How GLEngine finds the driver** (libGFXShared `gfxPluginConnectAll` 0xe58):
  - For each accelerator service passed to `gliInitializeLibrary`, it reads the `IOGLESBundleName` registry property.
  - It builds the path `/System/Library/Extensions/<name>.bundle/<name>`, dlopens it, and dlsyms about 80 `gld*` symbols
    (79 names in the string table) into a 0x248-byte plugin record.
  - There are two anchors for that record: +0x11C = `gldChoosePixelFormat`, as used by `gliChoosePixelFormat`, and
    +0x14C = `gldSetInteger`, as used by `gliPresentViewES` with pname 0x2C3.
  - *Inferred: the remaining slots follow the string order from +0x114.*
  - **With no IOAcceleratorES service, no plugin loads, `gliGetVersion` returns 0, and OpenGLES dlcloses GLEngine.**
    GLEngine has no software renderer of its own. `gliCreateContext` can add a `_gfx_float_device_id` device only when
    api_bits bit 0 is set, and EAGL never sets it.
- **What IMGSGX535GLDriver exports:** 83 functions, all `gld*`:
  - object lifetimes: textures, buffers, framebuffers, vertex arrays, pipeline programs, queries, fences
  - `gldInitDispatch` / `gldUpdateDispatch`, which fill a *separate, small* GLD draw dispatch (0x9d80). It is not the 826-slot table.
  - `gldCreatePipelineProgram`, which consumes libGLProgrammability IR (`_glpPPShaderLinearize`) and libGLImage pixel
    conversion (`_glg*`).
  - The driver sees GLEngine's private state and shader IR, never GL calls.
- **Forwarding at the driver layer would mean:**
  - a fake kernel `IOAcceleratorES` service with an `IOGLESBundleName` property, since discovery goes through the registry
  - reimplementing about 80 gld entries against undocumented GLEngine structs
  - lowering Apple's GLSL IR back to host GLSL
  
  That is much harder than the GLI layer.
- **Recommendation: replace `OpenGLES.framework/GLEngine.bundle/GLEngine` with a GLI shim.** It is a plain rootfs file,
  so **no shared-cache patch is needed**. The shim should:
  1. Export the 19 `gli*` symbols. The unused ones can be stubs returning 0.
  2. Have `gliGetVersion` return 1, so no IOAcceleratorES is needed; `_eagl_init` calls `gliInitializeLibrary(0,0,0,…)`
     and carries on.
  3. Have `gliChoosePixelFormat` return one node of at least 0x34 bytes. Its flags bit 0x100 is a switch: set it and CA's
     ES2 compositor also runs through the host GL; clear it and CA stays software while apps still get contexts, because
     apps default to Accelerated=NO.
  4. Have `gliCreateContext` fill all 826 front slots with forwarding thunks, choosing ES1 or ES2 from api_bits 4/8.
     This is the mbxshim model; reuse `genstubs.py` with N=826. Core ES2 functions all sit below 761, so they keep the
     3.1.3 wire numbering.
  5. Implement `gliSetInteger` pnames 0x38E/0x399/0x39B (IOSurface-backed renderbuffer) and 0x2C1 (swap notify),
     `gliBindViewES`, and `gliPresentViewES`. On present, render into the surface from `drawable->vt[3]`, then call
     `drawable->vt[4](drawable,1)`, as mbxshim already does.
  
  This one shim serves ES1 and ES2 and makes both the MBX string patch and mbxshim unnecessary on the iPad.
  Keep the `sgx` node out of the DT either way.
- iPhoneSimulator 3.2 SDK: not examined. The questions were answered from the device binaries.

---

## Contradictions with earlier research
- **"1652-entry dispatch"** (gap-display §4): it is **two 826-entry** tables. The 3.1.3 table had 822 entries.
- **"calloc 0x19C0"** (web-sgx §1.4): that is 3.1.3's size. 3.2.2 callocs **0x19E0** for the context and 0x1A18 for the
  sharegroup. The "0x19e4/0x1a14" noted in gap-display are sharegroup offsets, not allocation sizes.
- **"X+0xCE8 vs X+0xCF8"**: resolved. It is the back-dispatch pointer, and it moved because 4 slots were added
  (3 inserted at 761, 1 appended).
- **"exactly the 3.1.3 MBX ABI"** (gap-display §4): the interface table and CreateGC are the same, but the **dispatch
  numbering is not**, and **MBX is ES1-only**. Patching the string alone would crash, or call the wrong function, for any
  fixed-point, OES or ortho/frustum call.
- **"portrait produced by CA (GL or software+scaler/CPU rotate)"** (gap-display §3): wrong. UIKit/GS rotate windows by
  π/2; CA composes a 1024×768 frame with no rotation step.
- **"CPU fallback in its vtable caller not traced"**: traced. 0x309c9594 is `IOSurface::copy_iosurface`, called from
  `IOMFBDisplay::finish_update` for page copy-back, which ignores failure. There is no fallback, and none is needed if
  page flipping is off (`MBX2D_PAGE_FLIP=0`).
- web-sgx §2 "EAGLContext fails cleanly": true, but it is **retried without being cached** (§1.5). Set `CA_ENABLE_OGL=0`.
- web-sgx §1.4 said `CFBundleGetFunctionPointerForName`. 7B500 uses `CFBundleGetDataPointerForName`, which has the same effect.
- The recommendation changes: prefer a GLI (GLEngine-replacement) shim over "cache string patch + mbxshim".
