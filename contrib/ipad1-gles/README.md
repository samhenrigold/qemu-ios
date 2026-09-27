# ipad1-gles — GLI engine for iOS 3.2.2 (iPad1,1, 7B500)

Replaces `/System/Library/Frameworks/OpenGLES.framework/GLEngine.bundle/GLEngine`.
It forwards ES 1.1 and ES 2.0 calls to the host GL executor (`hw/arm/gles-host.c`)
over mbxshim's guest-services channel (`mcr p15,3,…,c15,c15,0`, `QC_GLES`). The ABI is
described in `docs/ipad1/userland-gl-display.md` §3. Keep the `sgx` node out of the
device tree.

    ./build.sh      # -> GLEngine (armv7 MH_BUNDLE, -marm, ldid -S) + offline checks

| file | role |
|---|---|
| `glishim.c` | the 19 `gli*` entry points; `#include`s `../it-gles/mbxshim.c` for the host-call code, ES1 handlers, CA present, IOSurface binding and swap notification |
| `gligen.py` | `docs/ipad1/gli-dispatch-7B500.tsv` → `gli_fwd.h` (826 slots); `--check` self-test |
| `test_glishim.c` | host-native self-check: signatures (compile-time), all 826 slots filled, remap, lifetimes |

## Dispatch table (826 slots, front and back tables filled identically)

Slots are filled in this order of priority:
1. **mbxshim's hand-written ES1 handlers** (about 130). They cover texture/buffer
   uploads, loading the pages behind their pointers, `glPixelStorei` alignment, and so
   on. 3.1.3 slot *n* ≥ 761 is placed at 3.2 slot *n*+3. Their host (**wire**) numbers
   are unchanged.
2. **Glishim overrides:**
   - `glGetString` (117) answers "OpenGL ES 2.0" / "OpenGL ES GLSL ES 1.00" for ES2
     contexts.
   - `glShaderSource` (595), `glBindAttribLocation` (630), `glGetAttribLocation` (632) and
     `glGetUniformLocation` (625) load the pages holding their strings before the host
     reads them.
3. **Generated forwarders** for every slot that OpenGLES exports or that the real
   GLEngine fills for ES1 or ES2: 271 in total. Each sends its arguments as 32-bit words
   under wire number `slot` (< 761) or `slot−3` (≥ 764). Argument counts come from the
   TSV prototypes, or from gl.h for the ES tail.
4. **Log-once stubs returning 0** for everything else (555): desktop-only slots, the
   three slots new in 3.2 (761 VertexAttribDivisor, 762/763 Draw*Instanced) and 825
   `glFramebufferParameteriAPPLE`, none of which has a wire number.

**Host side:** `gles-host.c` runs the ES2 slots on its desktop GL 2.1 context
(`gles_es2_call`/`gles_es2_draw`): shaders and programs pass through, with ES GLSL 1.00
fed to GLSL 1.20 minus `#version` and `precision`; attribute arrays are fetched from
guest memory at draw time like the ES1 client arrays; the ES2-only `glGet` names are
answered. Not done: `glGetUniform*v`, `glGetShaderSource`, `glShaderBinary`.
The iPad machine registers the guest-services trap for the GLES calls only
(`ipad1_qemu_call` in `hw/arm/ipad1.c`).

## gli* entry points

| entry | behaviour |
|---|---|
| gliInitializeLibrary / TerminateLibrary | no-op, so no IOAcceleratorES service is needed |
| gliGetVersion | 2, 3, 0x20000; returns 1 so EAGL keeps the engine loaded |
| gliChoosePixelFormat / DestroyPixelFormat | returns one 0x34-byte node. flags 0x100 (accelerated) is set only if `GLI_ACCELERATED` is set in the environment. Default off: CA stays in software and apps still get contexts |
| gliCreateContext | `share == NULL` creates a host sharegroup, which that context owns. Other contexts join the share context's sharegroup. api_bits 4 = ES1, 8 = ES2; if both are set, the share context's API is used |
| gliDestroyContext | forgets the CA view (EAGL owns the binding) and deletes the host context; the owner also deletes the sharegroup |
| gliSetInteger | 0x38E: attach IOSurface. For a renderbuffer it becomes the view surface and sets the host drawable size; for a texture it goes through `GLESBindCoreSurface`. 0x39B: detach. 0x2C1: swap notification (IOMFB selector 20). Anything else returns 0 |
| gliGetInteger | writes 0 |
| gliBindViewES | records the drawable; NULL clears it |
| gliPresentViewES | mbxshim `GLESPresentView`: render into the current surface, then `drawable->vt[4](d,1)`, then `vt[3]` next frame |
| QueryRendererInfo, DestroyRendererInfo, AttachDrawable(WithOptions), SwapBuffers, Get/Set/CopyAttributes | stubs (OpenGLES 3.2.2 never calls them). Each returns 10015, except DestroyRendererInfo, which returns 0 |

## Test apps and images

`build.sh` also builds `GLTest.app` (ES1: cyan quad on magenta) and `GLTest2.app`
(ES2 shader: blue quad on yellow) from `contrib/it-gles/glapp.c`; both log to
`/dev/console`, i.e. the serial log. `imgtools/ipad1_rootfs.py build --gles` installs
the engine and the apps, `--ca-ogl` additionally lets SpringBoard's CoreAnimation
composite through GL (drops `CA_ENABLE_OGL=0`, sets `GLI_ACCELERATED=1`). The apps
only get icons on the **jailbroken** base; the pristine installd/SpringBoard hide
ldid-signed bundles. Boot with `amfi_allow_any_signature=1 cs_enforcement_disable=1`.
`tests/ipad1/gl-drive.py` boots a store on an overlay and scripts taps and screendumps.

## Results (2026-09-27)

- ES1 and ES2 apps render and CA composites them (software CA):
  `docs/ipad1/screens/2026-09-27-gles1-gltest.png`, `...-gles2-gltest2.png`.
  Presents 300+/300+ ok, no unhandled slots.
- Observed order: `gliBindViewES(drawable)` comes **before** the `0x38E` attach.
- Accelerated CA (`--ca-ogl`): SpringBoard gets an ES2 context and draws the home
  screen on the host into its IOSurface render target (read back into the guest on
  every framebuffer unbind: non-black content), but the screen stays black. The IOMFB
  swaps never complete (`IOMFB fCommandPool->getCommand(false) returned NULL` on
  serial, display layer never enabled), and after ~68 frames CA deletes its target.
  `-[EAGLContext swapNotification:...]` would send `gliSetInteger(0x2C1)` for an
  accelerated pixel format, but it is never called. Unresolved: why CA's
  `add_swap_token` path is not reached. Software CA stays the default.
