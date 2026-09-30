"""Frozen bake reference for FirmwareKit comparisons; no device builder. Source: 0be1499f24."""
import os, sys, plistlib, shutil
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path[:0] = [os.path.join(ROOT, "imgtools"), os.path.join(ROOT, "contrib/it-gles"), os.path.join(ROOT, "contrib/guest-package")]

OPENGLES = "System/Library/Frameworks/OpenGLES.framework/OpenGLES"


DYLD_CACHE = "System/Library/Caches/com.apple.dyld/dyld_shared_cache_armv6"


GL_FRONT_END = "contrib/gles-public/OpenGLES"


def gl_front_end(mnt, owners):
    """The GL front end over the framework binary (FirmwareKit's SystemEdits.installCAOGL, which proves the fit
    first), + dyld's override switch where OpenGLES is cached. Returns the report line."""
    from ipad1_rootfs import gli_uncache, gli_dispatch_info, DYLD_OVERRIDE
    src = os.path.join(ROOT, GL_FRONT_END)
    if not os.path.exists(src):
        raise SystemExit("%s missing (run contrib/gles-public/build.sh)" % src)
    cache = os.path.join(mnt, DYLD_CACHE)
    status = gli_uncache(mnt, OPENGLES, DYLD_CACHE) if os.path.exists(cache) else "no shared cache: the stock file replaced"
    if "overridden" in status:
        owners.append(("0 0", DYLD_OVERRIDE))
    shutil.copyfile(src, os.path.join(mnt, OPENGLES))
    os.chmod(os.path.join(mnt, OPENGLES), 0o755)
    owners.append(("0 0", OPENGLES))
    return "GL front end %s (%s; %s)" % (os.path.basename(src), status,
                                         gli_dispatch_info(cache) if os.path.exists(cache) else "no dispatch layout needed")

