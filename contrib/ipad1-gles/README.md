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
   - `glShaderSource` (592), `glBindAttribLocation` (527), `glGetAttribLocation` (529) and
     `glGetUniformLocation` (623) load the pages holding their strings before the host
     reads them.
3. **Generated forwarders** for every slot that OpenGLES exports or that the real
   GLEngine fills for ES1 or ES2: 271 in total. Each sends its arguments as 32-bit words
   under wire number `slot` (< 761) or `slot−3` (≥ 764). Argument counts come from the
   TSV prototypes, or from gl.h for the ES tail.
4. **Log-once stubs returning 0** for everything else (555): desktop-only slots, the
   three slots new in 3.2 (761 VertexAttribDivisor, 762/763 Draw*Instanced) and 825
   `glFramebufferParameteriAPPLE`, none of which has a wire number.

**Host gap:** `gles-host.c` implements the ES1 subset only. The ES2 forwarders
(shaders, programs, uniforms, vertex attribs, …) reach its once-per-slot `UNHANDLED`
warning and return 0 until it has an ES2 executor.

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

## Unverified (no emulated userland yet)

- The ES1/ES2 choice when api_bits has both 4 and 8 set.
- Whether EAGL expects real values from `gliGetInteger`.
- The order of `0x38E` relative to `gliBindViewES` for the second and later
  `renderbufferStorage:fromDrawable:`.
