/*
 * gldshim -- the gld driver plugin libGFXShared needs on iOS 4.x, installed as
 * /System/Library/Frameworks/OpenGLES.framework/GLRendererFloatQEMU.bundle/GLRendererFloatQEMU
 *
 * 4.x's EAGL creates a sharegroup only if libGFXShared's gfxCreateSharedState
 * finds a gld plugin and a device for the pixel format's renderer ID, and has
 * that plugin's gldCreateShared succeed. The real plugin, IMGSGX535GLDriver,
 * is found through the SGX IOAccelerator's IOGLESBundleName; without an SGX
 * there is none. libGFXShared's other registration path is the one used here:
 * gfxPluginConnectAll also loads every GLRendererFloat* bundle in OpenGLES's
 * resources directory, dlsyms the gld* entry points, and makes one device per
 * plugin (renderer ID GLD_RENDERER | 0x20000, device ID that | 1 << 24).
 *
 * The GL itself stays in glishim (the GLEngine replacement): it runs
 * gfxInitializeLibrary + gfxPluginConnectAll as the stock GLEngine does, and
 * nothing but libGFXShared's shared-state bookkeeping ever calls in here. So
 * the plugin answers the version check and the shared-state pair, and every
 * other gld* entry point is a stub that reports itself once and fails.
 * libGFXShared drops a plugin that lacks any of the 79 names it dlsyms
 * (ipad1_rootfs.py checks the firmware's list against this file's exports).
 */
extern long write(int, const void *, unsigned long);
extern void *calloc(unsigned long, unsigned long);
extern void free(void *);

#define GLD_RENDERER 0x7000        /* the SGX plugin's; only bits 8-15 may be set */
#define GLD_ERR 10015              /* kCGLBadRendererInfo-style failure */

static void say(const char *s)
{
    unsigned long n = 0;
    while (s[n]) n++;
    write(2, s, n);
}

void gldInitializeLibrary(void *svcs, unsigned z, unsigned mask, void *flush, void *bind, int init)
{
    (void)svcs; (void)z; (void)mask; (void)flush; (void)bind; (void)init;
    say("[gldshim] gldInitializeLibrary\n");
}

void gldTerminateLibrary(void) {}

/* libGFXShared requires 3.1.0 and a renderer ID with only bits 8-15 set. */
int gldGetVersion(int *major, int *minor, int *rev, unsigned *renderer)
{
    *major = 3; *minor = 1; *rev = 0; *renderer = GLD_RENDERER;
    return 1;
}

/* gfxCreateSharedState(ids, n): gldCreateShared(&slot, device mask, 4) per ID. */
int gldCreateShared(void **out, unsigned mask, unsigned n)
{
    (void)mask; (void)n;
    if (!out || !(*out = calloc(1, 16))) return GLD_ERR;
    return 0;
}

int gldDestroyShared(void *shared) { free(shared); return 0; }

#define STUB(name) \
    int name(void) { static int seen; if (!seen++) say("[gldshim] unexpected call: " #name "\n"); return GLD_ERR; }
STUB(gldGetRendererInfo) STUB(gldChoosePixelFormat) STUB(gldDestroyPixelFormat)
STUB(gldCreateContext) STUB(gldReclaimContext) STUB(gldDestroyContext) STUB(gldAttachDrawable)
STUB(gldInitDispatch) STUB(gldUpdateDispatch) STUB(gldUpdateFramebuffers) STUB(gldGetString)
STUB(gldGetError) STUB(gldSetInteger) STUB(gldGetInteger) STUB(gldReadPixels) STUB(gldFlush)
STUB(gldFinish) STUB(gldTestObject) STUB(gldFlushObject) STUB(gldFinishObject) STUB(gldWaitObject)
STUB(gldCreateTexture) STUB(gldIsTextureResident) STUB(gldModifyTexture) STUB(gldGenerateTexMipmaps)
STUB(gldLoadTexture) STUB(gldUnbindTexture) STUB(gldReclaimTexture) STUB(gldDestroyTexture)
STUB(gldCopyTexSubImage) STUB(gldModifyTexSubImage) STUB(gldGetTextureLevelInfo)
STUB(gldGetTextureLevelImage) STUB(gldCreateBuffer) STUB(gldBufferSubData) STUB(gldLoadBuffer)
STUB(gldFlushBuffer) STUB(gldPageoffBuffer) STUB(gldUnbindBuffer) STUB(gldReclaimBuffer)
STUB(gldDestroyBuffer) STUB(gldGetMemoryPlugin) STUB(gldSetMemoryPlugin) STUB(gldTestMemoryPlugin)
STUB(gldFlushMemoryPlugin) STUB(gldDestroyMemoryPlugin) STUB(gldCreateFramebuffer)
STUB(gldLoadFramebuffer) STUB(gldUnbindFramebuffer) STUB(gldReclaimFramebuffer)
STUB(gldDestroyFramebuffer) STUB(gldCreatePipelineProgram) STUB(gldGetPipelineProgramInfo)
STUB(gldModifyPipelineProgram) STUB(gldUnbindPipelineProgram) STUB(gldDestroyPipelineProgram)
STUB(gldCreateProgram) STUB(gldDestroyProgram) STUB(gldCreateVertexArray) STUB(gldModifyVertexArray)
STUB(gldFlushVertexArray) STUB(gldUnbindVertexArray) STUB(gldReclaimVertexArray)
STUB(gldDestroyVertexArray) STUB(gldCreateFence) STUB(gldSetFence) STUB(gldDestroyFence)
STUB(gldCreateQuery) STUB(gldModifyQuery) STUB(gldGetQueryInfo) STUB(gldDestroyQuery)
STUB(gldObjectPurgeable) STUB(gldObjectUnpurgeable) STUB(gldCreateComputeContext)
STUB(gldDestroyComputeContext) STUB(gldDiscardFramebuffer)
