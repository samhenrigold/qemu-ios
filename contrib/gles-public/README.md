# gles-public: one OpenGLES.framework for every emulated iOS

`OpenGLES` replaces `/System/Library/Frameworks/OpenGLES.framework/OpenGLES` whole, on the iPad 1 (3.2 to 5.1.1) and
the iPod touch 2G (2.x to 4.2.1): one fat armv6 + armv7 binary, byte for byte the same everywhere. Nothing under
OpenGLES (GLEngine, MBXGLEngine, libGFXShared, a gld plugin) is loaded. GL calls reach the host executor
(`hw/arm/gles-host.c`) over the mbxshim core's guest-services trap, keyed by `include/hw/arm/guest-services/gles-names.h`.

The seam is the public API because that is what every GL consumer in every firmware uses and what barely moves between
releases: `docs/ipad1/gles-public-seam.md` has the survey (every symbol and selector, per build) and the disassembly
behind the private pieces.

    ./build.sh [OUT]     # -> OpenGLES (default here); checks each slice exports exactly opengles.exports

| file | role |
|---|---|
| `opengles.c` | the front end: a forwarder per export, 2.x's EGL, EAGL with every method any build has |
| `opengles.exports` | every name any 2.2.1-5.1.1 OpenGLES exports (the union; dyld binds cached consumers by name) |
| `gligen.c`, `gligen.sh` | the name table check (`gligen.sh`; `--stamp` after a hand edit of gles-names.h) |

## What is found at run time

Nothing is chosen by build number:

- **The layer's drawable.** `renderbufferStorage:fromDrawable:` binds `-[CAEAGLLayer nativeWindow]`. Its `version`
  word says whether there is 4.x's seventh entry, `properties`, whose bit 2 transposes the renderbuffer.
- **Surfaces.** IOSurface where the firmware has it, else CoreSurface (2.x). `attachImage:toCoreSurface:invertedRender:`
  binds a surface as a texture, or as the renderbuffer when the target is `GL_RENDERBUFFER`. 4.x+'s `texImageIOSurface:…`
  binds one as a texture, and a surface with no pixel format takes its layout from the GL format/type.
- **The swap.** `swapNotification:` does what stock EAGL does for a context the GPU doesn't complete swaps for:
  `IOMobileFramebufferSwapSignal` on the framebuffer it is given. `sendNotification:` names the framebuffer by ID, so it
  signals the main display when that display's ID matches, and otherwise counts a refusal.
- **5.x's macro context.** `GetMacroContextPrivate` returns `{GC, table}`. The table is laid out as this firmware's
  `__GLIFunctionDispatchRec`, whose @encode is read from the stock OpenGLES image in the mapped shared cache
  (`shared_region_check_np`). 5.x QuartzCore and CoreImage call GL only through it.
- **ES 2.0** where the firmware ships the SGX engine (`OpenGLES.framework/GLEngine.bundle`); ES 1.1 everywhere.
- **What the GPU reports.** The MBX where there is no GLEngine.bundle, else the SGX 535. `glGetString` answers from the
  firmware's own engine, in the cache or else its file: the renderer and version from MBXGLEngine or the SGX driver
  ("OpenGL ES-CM 1.1 (48)", "OpenGL ES 2.0 IMGSGX535-63.24"), the extensions the hardware reports, the bridge
  implements and the engine names (`fe_ext_table` lists those left out). `glGet` clamps the host's limits to the
  GPU's (`fe_limit_table`: MBX 1024-texel textures and 2 units; SGX 2048, 8 units, ES 2.0's 128/64/8 vectors).

FirmwareKit's `FitCheck.glesFrontEnd` proves at prepare that each of these exists in the firmware, and a misfit fails
the prepare. The first CoreAnimation use prints `[gles] CoreAnimation composites through the host (first …)`: the
matrix `gl` column requires that line, because a software CoreAnimation draws the same
pictures.

## Linking

Legacy-linked (`contrib/armv6-toolchain` `LEGACY_LINK=1`: no `LC_DYLD_INFO_ONLY`, classic relocations, r9 reserved as
2.x's thread pointer), so 2.x's and 3.0's dyld load it, and newer dyld reads it too. The EAGL classes are static ObjC 2
class data bound two-level to CoreFoundation's `NSObject` and libobjc's messengers, as on every firmware from 2.0.
Where OpenGLES is in the shared cache (3.1+), dyld's own `enable-dylibs-to-override-cache` switch makes it load the file,
and dyld rebinds the cached consumers' imports to it.

1.x is not covered: it has LayerKit and the old ObjC runtime. `contrib/it-gles/gles1x.c` (`OpenGLES-1x`) still serves it
(`docs/ipad1/gles-public-seam.md`, "1.x").
