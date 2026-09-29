/*
 * gldshim -- the gld driver plugin libGFXShared needs on iOS 4.x and 5.x, installed as
 * /System/Library/Frameworks/OpenGLES.framework/GLRendererFloatQEMU.bundle/GLRendererFloatQEMU
 *
 * 4.x and 5.x EAGL create a sharegroup only if libGFXShared's gfxCreateSharedState
 * finds a gld plugin and a device for the pixel format's renderer ID, and has
 * the plugin make its per-device shared object. The real plugin, IMGSGX535GLDriver,
 * is found through the SGX IOAccelerator's IOGLESBundleName; without an SGX
 * there is none. libGFXShared's other registration path is the one used here:
 * gfxPluginConnectAll also loads every GLRendererFloat* bundle in OpenGLES's
 * resources directory, dlsyms the gld* entry points, and makes one device per
 * plugin (renderer ID GLD_RENDERER | 0x20000, device ID that | 1 << 24).
 *
 * The GL itself stays in glishim (the GLEngine replacement): it runs
 * gfxInitializeLibrary + gfxPluginConnectAll as the stock GLEngine does, and
 * nothing but libGFXShared's shared-state bookkeeping ever calls in here. So
 * the plugin answers the version check, device creation and the shared-object
 * pair, and every other gld* entry point is a stub that reports itself once and
 * fails. libGFXShared drops a plugin that lacks any name it dlsyms (4.x: 79,
 * 5.x: 111; ipad1_rootfs.py checks the firmware's list against this file's
 * exports). The two generations (gfx_gen.h), as each libGFXShared checks them:
 *   4.x  gldGetVersion 3.1.0; gldCreateShared(&slot, device mask, n) per ID
 *   5.x  gldGetVersion 4.0.44; gldCreateDevice(&device, ...) per registered device,
 *        then gldCreateShareGroup(device, &slot, n) / gldDestroyShareGroup(slot)
 */
#include "gfx_gen.h"

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

/* 4.x libGFXShared requires 3.1.0, 5.x 4.0.44; both a renderer ID with only bits 8-15 set. */
int gldGetVersion(int *major, int *minor, int *rev, unsigned *renderer)
{
    int five = gfx_generation() == 5;
    *major = five ? 4 : 3; *minor = five ? 0 : 1; *rev = five ? 44 : 0; *renderer = GLD_RENDERER;
    return 1;
}

/* 5.x: one per device libGFXShared registers; its result is the first argument of gldCreateShareGroup. */
static int gld_device;
int gldCreateDevice(void **out, unsigned id, void *info)
{
    (void)id; (void)info;
    if (out) *out = &gld_device;
    return 0;
}

int gldDestroyDevice(void *device) { (void)device; return 0; }

int gldCreateShareGroup(void *device, void **out, unsigned n)
{
    (void)device; (void)n;
    if (!out || !(*out = calloc(1, 16))) return GLD_ERR;
    return 0;
}

int gldDestroyShareGroup(void *group) { free(group); return 0; }

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
/* 5.x's additions */
STUB(gldPopulateRendererInfo) STUB(gldPopulateContextDispatch) STUB(gldUpdateReadFramebuffer)
STUB(gldUpdateDrawFramebuffer) STUB(gldGetDeviceString) STUB(gldClearFramebufferData)
STUB(gldBlitFramebufferData) STUB(gldReadFramebufferData) STUB(gldPresentFramebufferData)
STUB(gldFlushContext) STUB(gldFinishContext) STUB(gldWaitForContext) STUB(gldWaitForObject)
STUB(gldRestoreTextureData) STUB(gldCopyBufferSubData) STUB(gldRestoreBufferData)
STUB(gldCreateSampler) STUB(gldDestroySampler) STUB(gldCreateQueue) STUB(gldDestroyQueue)
STUB(gldFlushQueue) STUB(gldFinishQueue) STUB(gldCreateComputeProgram) STUB(gldDestroyComputeProgram)
STUB(gldUpdateComputeProgram) STUB(gldWriteComputeProgramBinary) STUB(gldCreateKernel)
STUB(gldDestroyKernel) STUB(gldSubmitKernel) STUB(gldSubmitNativeKernel)
STUB(gldReadBufferDataWithQueue) STUB(gldReadTextureDataWithQueue) STUB(gldWriteBufferDataWithQueue)
STUB(gldWriteTextureDataWithQueue) STUB(gldCopyBufferDataWithQueue) STUB(gldCopyTextureDataWithQueue)
STUB(gldCopyBufferDataToTextureWithQueue) STUB(gldCopyTextureDataToBufferWithQueue)
STUB(gldSubmitFenceOnQueue) STUB(gldGetFenceStatusOnQueue) STUB(gldWaitForFenceOnQueue)
