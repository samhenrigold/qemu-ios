"""Frozen bake reference for FirmwareKit comparisons; no device builder. Source: 0be1499f24."""
import os, sys, plistlib, shutil
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path[:0] = [os.path.join(ROOT, "imgtools"), os.path.join(ROOT, "contrib/it-gles"), os.path.join(ROOT, "contrib/guest-package")]

OPENGLES = "System/Library/Frameworks/OpenGLES.framework/OpenGLES"


SPRINGBOARD_JOB = "System/Library/LaunchDaemons/com.apple.SpringBoard.plist"


GUEST_PACKAGE = os.path.join(ROOT, "build/guest-package/armv6.itpack")


EXPORTS = os.path.join(ROOT, "contrib/it-gles/opengles-1x.exports")


def gles2x_front_end(mnt):
    """(True, line) if the stock OpenGLES exports exactly opengles-1x.exports, else (False, why)."""
    import gles2x_exports
    stock = os.path.join(mnt, OPENGLES)
    if not os.path.exists(stock):
        return False, "no %s" % OPENGLES
    if os.path.exists(stock + ".baked"):
        stock += ".baked"
    want, got = set(gles2x_exports.read_list(EXPORTS)), set(gles2x_exports.scan(stock))
    if want != got:
        return False, "stock OpenGLES exports differ from opengles-1x.exports (missing %s, extra %s): stock kept" % (
            sorted(want - got)[:4], sorted(got - want)[:4])
    return True, "GL front end replaces OpenGLES (%d exports, the firmware's own)" % len(got)


def springboard_env(mnt, ogl):
    path = os.path.join(mnt, SPRINGBOARD_JOB)
    data = open(path, "rb").read()
    job = plistlib.loads(data)
    env = job.setdefault("EnvironmentVariables", {})
    if ogl:
        env.update(LK_ENABLE_OGL="1", LK_AUTO_ENABLE_OGL="0")
    else:
        env.pop("LK_ENABLE_OGL", None)
        env.pop("LK_AUTO_ENABLE_OGL", None)
    env["LK_ENABLE_MBX2D"] = "0"            # never the unemulated MBX 2D path (devos50's image already says so)
    open(path, "wb").write(plistlib.dumps(job, fmt=plistlib.FMT_BINARY if data.startswith(b"bplist") else plistlib.FMT_XML))


def bake(mnt, itpack=GUEST_PACKAGE, gles=True):
    """Bake the mounted 1.x system volume; returns (report, root-owned volume-relative paths)."""
    import mkpkg
    report, owners = {}, [SPRINGBOARD_JOB]
    front, why = gles2x_front_end(mnt) if gles else (False, "gles off")
    seeded, report["guest_package"] = mkpkg.seed(mnt, itpack, front)
    if front and "/" + OPENGLES not in report["guest_package"]["hooks"]:
        raise SystemExit("%s has no OpenGLES hook for this build; rebuild contrib/guest-package" % itpack)
    springboard_env(mnt, front)
    report["gles"] = why + ("; LayerKit composites through it (LK_ENABLE_OGL=1)" if front else "; software LayerKit")
    report["gles_engine"] = "OpenGLES" if front else None
    return report, owners + seeded

