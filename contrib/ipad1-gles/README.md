# ipad1-gles — GLI engine for the iPad 1 (iOS 3.2 / 3.2.2, and 4.2.1)

Replaces `/System/Library/Frameworks/OpenGLES.framework/GLEngine.bundle/GLEngine`.
It forwards ES 1.1 and ES 2.0 calls to the host GL executor (`hw/arm/gles-host.c`)
over mbxshim's guest-services channel (`mcr p15,3,…,c15,c15,0`, `QC_GLES`). The ABI is
described in `docs/ipad1/userland-gl-display.md` §3. Keep the `sgx` node out of the
device tree.

    ./build.sh      # -> GLEngine-<BUILD> per dispatch TSV, GLRendererFloatQEMU.bundle, test apps, checks

One engine per dispatch layout: `docs/ipad1/gli-dispatch-<BUILD>.tsv` (7B500's also serves 7B367;
8C148 has 841 slots), generated from a firmware's shared cache by `glitsv.py CACHE BUILD OUT.tsv`
(`--verify CACHE TSV` checks one). `imgtools/ipad1_rootfs.py build` installs the engine whose TSV matches
the firmware's `__GLIFunctionDispatchRec`. On 4.x it also installs the gld plugin and dyld's
`enable-dylibs-to-override-cache` switch (GLEngine is in the 4.x shared cache). The 4.x contract and
design are in `docs/ipad1/ios4.md`, "GL CoreAnimation on 4.2.1".

| file | role |
|---|---|
| `glishim.c` | the 19 `gli*` entry points; `#include`s `../it-gles/mbxshim.c` for the host-call code, ES1 handlers, CA present, IOSurface binding and swap notification |
| `gligen.py` | `docs/ipad1/gli-dispatch-<BUILD>.tsv` (`--tsv`) → `gli_fwd.h`; `--check` self-test |
| `glitsv.py` | a firmware's dispatch TSV from its shared cache (slots from the @encode, exports from the trampolines, the rest carried from 7B500's by field name) |
| `gldshim.c` | 4.x only: the gld plugin libGFXShared needs before EAGL makes a context (`GLRendererFloatQEMU.bundle`) |
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

**Guest pointers:** every host access behind a pointer a GL call passes (client arrays, indices, texels,
buffer data, parameter arrays, glGet*/glGen*/glReadPixels outputs) goes through `gles_guest_rw`
(`hw/arm/guest-gles.c`). Inside an unbatched call it probes each page through the caller's MMU; a page
that would fault (never touched, copy-on-write, read-only for a store) fails the call, no draw is issued,
and the fault is raised as a data abort on the trapping `mcr` (`ARM_CP_RAISES_EXC`), so the kernel pages
it in and the call is reissued, as the CPU loads a real GL would do. A static const vertex table works
without the shim touching it. The shim's page touches (uploads, strings, present) stay as a prefetch.

## gli* entry points

| entry | behaviour |
|---|---|
| gliInitializeLibrary / TerminateLibrary | 3.2.x: no-op. 4.x (libGFXShared loaded by OpenGLES): `gfxInitializeLibrary` + `gfxPluginConnectAll` as the stock engine, then checks gldshim's device 0x01027000 is registered |
| gliGetVersion | 2, 3, 0x20000; returns 1 so EAGL keeps the engine loaded |
| gliChoosePixelFormat / DestroyPixelFormat | returns one 0x34-byte node. flags 0x100 (accelerated) is set if `GLI_ACCELERATED` is set in the environment (3.2.x: default off, CA stays in software and apps still get contexts) or gldshim is registered (4.x EAGL loads only accelerated formats); the renderer is then gldshim's device |
| gliCreateContextWithShared | 4.x: every context of an EAGL sharegroup; contexts with the same pixel format pointer share one host sharegroup, freed with the last |
| gliCreateContext | `share == NULL` creates a host sharegroup, which that context owns. Other contexts join the share context's sharegroup. api_bits 4 = ES1, 8 = ES2; if both are set, the share context's API is used |
| gliDestroyContext | forgets the CA view (EAGL owns the binding) and deletes the host context; the owner also deletes the sharegroup |
| gliSetInteger | 0x38E: attach IOSurface. For a renderbuffer it becomes the view surface and sets the host drawable size; for a texture it goes through `GLESBindCoreSurface`. 0x39B: detach. 0x2C1: swap notification (`IOMobileFramebufferSwapSignal` on the main display). Anything else returns 0 |
| gliGetInteger | writes 0 |
| gliBindViewES | returns nonzero on success (4.x EAGL returns it from `renderbufferStorage:fromDrawable:`). 3.2.x: records the drawable. 4.x: binds CA's drawable as the stock engine does (bind with a create/destroy/preflight block, first nextBuffer, 0x38E attach as GL_RENDERBUFFER). NULL unbinds the old one (`vt[2]`) and clears it, so a rebuilt framebuffer can bind the layer again |
| gliPresentViewES | takes the frame's buffer and touches its pages (saves the host a fault round trip per page), then mbxshim `GLESPresentView`: render into the current surface, then `drawable->vt[4](d,1)`, then `vt[3]` next frame |
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

## Results, 4.2.1 (2026-09-28)

GL CA is the 8C148 default: the "Connect to iTunes" screen composites through the host GL. The fixture
job `contrib/it-gltest` (baked by `ipad1_device.py create --gl-test`, read by `tests/ipad1/gltest.py`)
checks GL without an app launch: readback PASS, colour fractions within 7% of the layer's, 61.5 presents
per second, no torn frames.

## Results (2026-09-27)

- ES1 and ES2 apps render and CA composites them (software CA):
  `docs/ipad1/screens/2026-09-27-gles1-gltest.png`, `...-gles2-gltest2.png`.
  Presents 300+/300+ ok, no unhandled slots.
- Observed order: `gliBindViewES(drawable)` comes **before** the `0x38E` attach.
- Accelerated CA (`--ca-ogl`) works: SpringBoard composites the home screen and
  both apps through the host GL (`docs/ipad1/screens/2026-09-27-ca-gl-home.png`,
  `...-ca-gl-gles2.png`). CA renders into an IOSurface-backed texture, read back
  into the guest when it unbinds the framebuffer; each IOMFB swap waits for a GPU
  token that EAGL requests with `gliSetInteger(0x2C1, {fbID, txn, layer})`, and the
  shim answers with `IOMobileFramebufferSwapSignal` on the main display. Without
  that the swaps never complete (`IOMFB fCommandPool->getCommand(false) returned
  NULL`) and the screen stays black. Software CA remains the image default.

## Coverage (generated)

Every entry point OpenGLES 3.2.2 exports, what the real GLEngine fills it for (ES1/ES2 columns),
and what happens to it here. `host` = executed by `hw/arm/gles-host.c`; `forwarded, host UNHANDLED` =
glishim sends it, the host logs `[gles] UNHANDLED slot` once and returns 0; `stub` = no host wire
number, glishim logs `[glishim] unimplemented GL entry point NAME` once. Regenerate with
`python3 gligen.py --coverage` after a host change.

Summary: forwarded, host UNHANDLED: 58, guest (glishim answers): 1, host: 208, stub (no wire slot): 1.

Gaps that matter most: the GL_FIXED getters and the fixed-point light/material/texenv/fog/
point-parameter setters (glLightx, glTexEnvx, glFogx, ...), buffer mapping (glMapBuffer returns a host
pointer and cannot work as a plain forward), the APPLE fence family, ES2 queries (glGetUniform*v,
glGetVertexAttribfv, glGetShaderSource), glIsBuffer and the palette-matrix OES extension. The ES 1.1
gap fills from the iPod main line (point size array, DrawTex, blend separate/equation, glLightf/
LightModel, glPointParameter, glClipPlanef, glLogicOp, glIsEnabled/glIsTexture, glCopyTexSubImage2D)
reach the iPad through mbxshim.c's thunks, which gli_fill places at the 3.2 slots.

| slot | entry point | ES1 | ES2 | status |
|---|---|---|---|---|
| 1 | glAlphaFunc | Y |  | host |
| 5 | glBindTexture | Y | Y | host |
| 7 | glBlendFunc | Y | Y | host |
| 10 | glClear | Y | Y | host |
| 12 | glClearColor | Y | Y | host |
| 15 | glClearStencil | Y | Y | host |
| 37 | glColor4f | Y |  | host |
| 43 | glColor4ub | Y |  | host |
| 49 | glColorMask | Y | Y | host |
| 51 | glColorPointer | Y |  | host |
| 54 | glCopyTexImage2D | Y | Y | host |
| 56 | glCopyTexSubImage2D | Y | Y | host |
| 57 | glCullFace | Y | Y | host |
| 59 | glDeleteTextures | Y | Y | host |
| 60 | glDepthFunc | Y | Y | host |
| 61 | glDepthMask | Y | Y | host |
| 63 | glDisable | Y | Y | host |
| 64 | glDisableClientState | Y |  | host |
| 65 | glDrawArrays | Y | Y | host |
| 67 | glDrawElements | Y | Y | host |
| 72 | glEnable | Y | Y | host |
| 73 | glEnableClientState | Y |  | host |
| 89 | glFinish | Y | Y | host |
| 90 | glFlush | Y | Y | host |
| 91 | glFogf | Y |  | host |
| 92 | glFogfv | Y |  | host |
| 95 | glFrontFace | Y | Y | host |
| 98 | glGenTextures | Y | Y | host |
| 99 | glGetBooleanv | Y | Y | host |
| 102 | glGetError | Y | Y | host |
| 103 | glGetFloatv | Y | Y | host |
| 104 | glGetIntegerv | Y | Y | host |
| 105 | glGetLightfv | Y |  | host |
| 110 | glGetMaterialfv | Y |  | host |
| 115 | glGetPointerv | Y |  | host |
| 117 | glGetString | Y | Y | guest (glishim answers) |
| 118 | glGetTexEnvfv | Y |  | host |
| 119 | glGetTexEnviv | Y |  | host |
| 123 | glGetTexImage | Y | Y | forwarded, host UNHANDLED |
| 124 | glGetTexLevelParameterfv |  |  | forwarded, host UNHANDLED |
| 125 | glGetTexLevelParameteriv | Y | Y | forwarded, host UNHANDLED |
| 126 | glGetTexParameterfv | Y | Y | host |
| 127 | glGetTexParameteriv | Y | Y | host |
| 128 | glHint | Y | Y | host |
| 143 | glIsEnabled | Y | Y | host |
| 145 | glIsTexture | Y | Y | host |
| 146 | glLightModelf | Y |  | host |
| 147 | glLightModelfv | Y |  | host |
| 150 | glLightf | Y |  | host |
| 151 | glLightfv | Y |  | host |
| 155 | glLineWidth | Y | Y | host |
| 157 | glLoadIdentity | Y |  | host |
| 159 | glLoadMatrixf | Y |  | host |
| 161 | glLogicOp | Y |  | host |
| 170 | glMaterialf | Y |  | host |
| 171 | glMaterialfv | Y |  | host |
| 174 | glMatrixMode | Y |  | host |
| 176 | glMultMatrixf | Y |  | host |
| 182 | glNormal3f | Y |  | host |
| 188 | glNormalPointer | Y |  | host |
| 195 | glPixelStorei | Y | Y | host |
| 199 | glPointSize | Y |  | host |
| 201 | glPolygonOffset | Y | Y | host |
| 205 | glPopMatrix | Y |  | host |
| 210 | glPushMatrix | Y |  | host |
| 237 | glReadPixels | Y | Y | host |
| 248 | glRotatef | Y |  | host |
| 250 | glScalef | Y |  | host |
| 251 | glScissor | Y | Y | host |
| 253 | glShadeModel | Y |  | host |
| 254 | glStencilFunc | Y | Y | host |
| 255 | glStencilMask | Y | Y | host |
| 256 | glStencilOp | Y | Y | host |
| 289 | glTexCoordPointer | Y |  | host |
| 290 | glTexEnvf | Y |  | host |
| 291 | glTexEnvfv | Y |  | host |
| 292 | glTexEnvi | Y |  | host |
| 293 | glTexEnviv | Y |  | host |
| 301 | glTexImage2D | Y | Y | host |
| 302 | glTexParameterf | Y | Y | host |
| 303 | glTexParameterfv | Y | Y | host |
| 304 | glTexParameteri | Y | Y | host |
| 305 | glTexParameteriv | Y | Y | host |
| 307 | glTexSubImage2D | Y | Y | host |
| 309 | glTranslatef | Y |  | host |
| 334 | glVertexPointer | Y |  | host |
| 335 | glViewport | Y | Y | host |
| 336 | glBlendFuncSeparate | Y | Y | host |
| 337 | glBlendColor |  | Y | host |
| 338 | glBlendEquation | Y | Y | host |
| 341 | glClientActiveTexture | Y |  | host |
| 342 | glActiveTexture | Y | Y | host |
| 369 | glMultiTexCoord4f | Y |  | host |
| 379 | glCompressedTexImage3D |  |  | forwarded, host UNHANDLED |
| 380 | glCompressedTexImage2D | Y | Y | host |
| 382 | glCompressedTexSubImage3D |  |  | forwarded, host UNHANDLED |
| 383 | glCompressedTexSubImage2D | Y | Y | host |
| 405 | glDrawRangeElements |  |  | forwarded, host UNHANDLED |
| 458 | glBlendEquationSeparate | Y | Y | host |
| 459 | glSampleCoverage | Y | Y | host |
| 463 | glGenFencesAPPLE | Y | Y | forwarded, host UNHANDLED |
| 464 | glDeleteFencesAPPLE | Y | Y | forwarded, host UNHANDLED |
| 465 | glSetFenceAPPLE | Y | Y | forwarded, host UNHANDLED |
| 466 | glIsFenceAPPLE | Y | Y | forwarded, host UNHANDLED |
| 467 | glTestFenceAPPLE | Y | Y | forwarded, host UNHANDLED |
| 468 | glFinishFenceAPPLE | Y | Y | forwarded, host UNHANDLED |
| 469 | glTestObjectAPPLE | Y | Y | forwarded, host UNHANDLED |
| 470 | glFinishObjectAPPLE | Y | Y | forwarded, host UNHANDLED |
| 476 | glVertexAttrib1f |  | Y | host |
| 479 | glVertexAttrib2f |  | Y | host |
| 482 | glVertexAttrib3f |  | Y | host |
| 485 | glVertexAttrib4f |  | Y | host |
| 489 | glVertexAttrib1fv |  | Y | host |
| 492 | glVertexAttrib2fv |  | Y | host |
| 495 | glVertexAttrib3fv |  | Y | host |
| 503 | glVertexAttrib4fv |  | Y | host |
| 511 | glVertexAttribPointer |  | Y | host |
| 512 | glEnableVertexAttribArray |  | Y | host |
| 513 | glDisableVertexAttribArray |  | Y | host |
| 515 | glGetVertexAttribfv |  | Y | forwarded, host UNHANDLED |
| 516 | glGetVertexAttribiv |  | Y | host |
| 517 | glGetVertexAttribPointerv |  | Y | host |
| 540 | glPointParameterf | Y |  | host |
| 541 | glPointParameterfv | Y |  | host |
| 586 | glStencilOpSeparate |  | Y | host |
| 591 | glDeleteShader |  |  | host |
| 593 | glDetachShader |  | Y | host |
| 594 | glCreateShader |  | Y | host |
| 595 | glShaderSource |  | Y | host |
| 596 | glCompileShader |  | Y | host |
| 597 | glCreateProgram |  | Y | host |
| 598 | glAttachShader |  | Y | host |
| 599 | glLinkProgram |  | Y | host |
| 600 | glUseProgram |  | Y | host |
| 601 | glValidateProgram |  | Y | host |
| 602 | glUniform1f |  | Y | host |
| 603 | glUniform2f |  | Y | host |
| 604 | glUniform3f |  | Y | host |
| 605 | glUniform4f |  | Y | host |
| 606 | glUniform1i |  | Y | host |
| 607 | glUniform2i |  | Y | host |
| 608 | glUniform3i |  | Y | host |
| 609 | glUniform4i |  | Y | host |
| 610 | glUniform1fv |  | Y | host |
| 611 | glUniform2fv |  | Y | host |
| 612 | glUniform3fv |  | Y | host |
| 613 | glUniform4fv |  | Y | host |
| 614 | glUniform1iv |  | Y | host |
| 615 | glUniform2iv |  | Y | host |
| 616 | glUniform3iv |  | Y | host |
| 617 | glUniform4iv |  | Y | host |
| 618 | glUniformMatrix2fv |  | Y | host |
| 619 | glUniformMatrix3fv |  | Y | host |
| 620 | glUniformMatrix4fv |  | Y | host |
| 625 | glGetUniformLocation |  | Y | host |
| 626 | glGetActiveUniform |  | Y | host |
| 627 | glGetUniformfv |  | Y | forwarded, host UNHANDLED |
| 628 | glGetUniformiv |  | Y | forwarded, host UNHANDLED |
| 629 | glGetShaderSource |  | Y | forwarded, host UNHANDLED |
| 630 | glBindAttribLocation |  | Y | host |
| 631 | glGetActiveAttrib |  | Y | host |
| 632 | glGetAttribLocation |  | Y | host |
| 634 | glGenQueries |  |  | forwarded, host UNHANDLED |
| 635 | glDeleteQueries |  |  | forwarded, host UNHANDLED |
| 636 | glIsQuery |  |  | forwarded, host UNHANDLED |
| 637 | glBeginQuery |  |  | forwarded, host UNHANDLED |
| 638 | glEndQuery |  |  | forwarded, host UNHANDLED |
| 639 | glGetQueryiv |  |  | forwarded, host UNHANDLED |
| 640 | glGetQueryObjectiv |  |  | forwarded, host UNHANDLED |
| 641 | glGetQueryObjectuiv |  |  | forwarded, host UNHANDLED |
| 642 | glBindBuffer | Y | Y | host |
| 643 | glDeleteBuffers | Y | Y | host |
| 644 | glGenBuffers | Y | Y | host |
| 645 | glIsBuffer | Y | Y | forwarded, host UNHANDLED |
| 646 | glBufferData | Y | Y | host |
| 647 | glBufferSubData | Y | Y | host |
| 648 | glGetBufferSubData |  |  | forwarded, host UNHANDLED |
| 649 | glMapBuffer | Y | Y | forwarded, host UNHANDLED |
| 650 | glUnmapBuffer | Y | Y | forwarded, host UNHANDLED |
| 651 | glGetBufferParameteriv | Y | Y | forwarded, host UNHANDLED |
| 652 | glGetBufferPointerv | Y | Y | forwarded, host UNHANDLED |
| 655 | glIsShader |  | Y | host |
| 656 | glIsProgram |  | Y | host |
| 657 | glGetShaderiv |  | Y | host |
| 658 | glGetProgramiv |  | Y | host |
| 659 | glGetShaderInfoLog |  | Y | host |
| 660 | glGetProgramInfoLog |  | Y | host |
| 661 | glStencilFuncSeparate |  | Y | host |
| 662 | glStencilMaskSeparate |  | Y | host |
| 665 | glIsRenderbuffer | Y | Y | host |
| 666 | glBindRenderbuffer | Y | Y | host |
| 667 | glDeleteRenderbuffers | Y | Y | host |
| 668 | glGenRenderbuffers | Y | Y | host |
| 669 | glRenderbufferStorage | Y | Y | host |
| 670 | glGetRenderbufferParameteriv | Y | Y | host |
| 671 | glIsFramebuffer | Y | Y | host |
| 672 | glBindFramebuffer | Y | Y | host |
| 673 | glDeleteFramebuffers | Y | Y | host |
| 674 | glGenFramebuffers | Y | Y | host |
| 675 | glCheckFramebufferStatus | Y | Y | host |
| 677 | glFramebufferTexture2D | Y | Y | host |
| 678 | glFramebufferTexture3D |  |  | forwarded, host UNHANDLED |
| 679 | glFramebufferRenderbuffer | Y | Y | host |
| 680 | glGetFramebufferAttachmentParameteriv | Y | Y | host |
| 681 | glGenerateMipmap | Y | Y | host |
| 759 | glGetAttachedShaders |  | Y | host |
| 764 | glAlphaFuncx | Y |  | host |
| 765 | glClearColorx | Y |  | host |
| 766 | glClearDepthf | Y | Y | host |
| 767 | glClearDepthx | Y |  | host |
| 768 | glClipPlanef | Y |  | host |
| 769 | glClipPlanex | Y |  | forwarded, host UNHANDLED |
| 770 | glColor4x | Y |  | host |
| 771 | glDepthRangef | Y | Y | host |
| 772 | glDepthRangex | Y |  | host |
| 773 | glFogx | Y |  | forwarded, host UNHANDLED |
| 774 | glFogxv | Y |  | forwarded, host UNHANDLED |
| 775 | glFrustumf | Y |  | host |
| 776 | glFrustumx | Y |  | host |
| 777 | glGetClipPlanef | Y |  | forwarded, host UNHANDLED |
| 778 | glGetClipPlanex | Y |  | forwarded, host UNHANDLED |
| 779 | glGetFixedv | Y |  | forwarded, host UNHANDLED |
| 780 | glGetLightxv | Y |  | forwarded, host UNHANDLED |
| 781 | glGetMaterialxv | Y |  | forwarded, host UNHANDLED |
| 782 | glGetTexEnvxv | Y |  | forwarded, host UNHANDLED |
| 783 | glGetTexParameterxv | Y |  | forwarded, host UNHANDLED |
| 784 | glLightModelx | Y |  | forwarded, host UNHANDLED |
| 785 | glLightModelxv | Y |  | forwarded, host UNHANDLED |
| 786 | glLightx | Y |  | forwarded, host UNHANDLED |
| 787 | glLightxv | Y |  | forwarded, host UNHANDLED |
| 788 | glLineWidthx | Y |  | host |
| 789 | glLoadMatrixx | Y |  | host |
| 790 | glMaterialx | Y |  | forwarded, host UNHANDLED |
| 791 | glMaterialxv | Y |  | forwarded, host UNHANDLED |
| 792 | glMultMatrixx | Y |  | host |
| 793 | glNormal3x | Y |  | host |
| 794 | glOrthof | Y |  | host |
| 795 | glOrthox | Y |  | host |
| 796 | glPointSizex | Y |  | host |
| 797 | glPolygonOffsetx | Y |  | host |
| 798 | glRotatex | Y |  | host |
| 799 | glScalex | Y |  | host |
| 800 | glTexEnvx | Y |  | forwarded, host UNHANDLED |
| 801 | glTexEnvxv | Y |  | forwarded, host UNHANDLED |
| 802 | glTexParameterx | Y |  | host |
| 803 | glTexParameterxv | Y |  | forwarded, host UNHANDLED |
| 804 | glTranslatex | Y |  | host |
| 805 | glMultiTexCoord4x | Y |  | host |
| 806 | glSampleCoveragex | Y |  | host |
| 807 | glPointParameterx | Y |  | forwarded, host UNHANDLED |
| 808 | glPointParameterxv | Y |  | forwarded, host UNHANDLED |
| 809 | glPointSizePointerOES | Y |  | host |
| 810 | glCurrentPaletteMatrixOES | Y |  | forwarded, host UNHANDLED |
| 811 | glLoadPaletteFromModelViewMatrixOES | Y |  | forwarded, host UNHANDLED |
| 812 | glMatrixIndexPointerOES | Y |  | forwarded, host UNHANDLED |
| 813 | glWeightPointerOES | Y |  | forwarded, host UNHANDLED |
| 814 | glDrawTexsOES | Y |  | host |
| 815 | glDrawTexiOES | Y |  | host |
| 816 | glDrawTexxOES | Y |  | host |
| 817 | glDrawTexsvOES | Y |  | host |
| 818 | glDrawTexivOES | Y |  | host |
| 819 | glDrawTexxvOES | Y |  | host |
| 820 | glDrawTexfOES | Y |  | host |
| 821 | glDrawTexfvOES | Y |  | host |
| 822 | glShaderBinary |  | Y | host |
| 823 | glGetShaderPrecisionFormat |  | Y | host |
| 824 | glReleaseShaderCompiler |  | Y | host |
| 825 | glFramebufferParameteriAPPLE | Y | Y | stub (no wire slot) |
