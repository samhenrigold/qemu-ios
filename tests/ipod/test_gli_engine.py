#!/usr/bin/env python3
"""ipod2g_device.gli_engine picks the iPod's GL engine from the volume: 3.1+ (a shared cache) the one
MBXGLEngine, 3.0 (the engine bundle a plain file, no cache, a dyld that refuses LC_DYLD_INFO_ONLY) its
legacy-linked build MBXGLEngine-30, 1.x/2.x (no bundle) none. And the package family for each build
carries that engine, signed."""
import os, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path[:0] = [os.path.join(ROOT, "imgtools"), os.path.join(ROOT, "contrib/guest-package")]
import ipod2g_device as d
import mkpkg

ENC = b'x{__GLIFunctionDispatchRec="accum"^?"alpha_func"^?}x'


def volume(t, *files):
    for rel in files:
        os.makedirs(os.path.dirname(os.path.join(t, rel)), exist_ok=True)
        open(os.path.join(t, rel), "wb").write(ENC)
    return t


with tempfile.TemporaryDirectory() as t:
    assert d.gli_engine(volume(t))[::2] == (False, None)                       # 2.x: no bundle
with tempfile.TemporaryDirectory() as t:
    ok, info, src = d.gli_engine(volume(t, d.MBX, d.OPENGLES))                 # 3.0
    assert ok and src == "contrib/it-gles/MBXGLEngine-30" and "2 slots" in info, (ok, info, src)
with tempfile.TemporaryDirectory() as t:
    ok, info, src = d.gli_engine(volume(t, d.MBX, d.OPENGLES, d.DYLD_CACHE))  # 3.1+
    assert ok and src == "contrib/it-gles/MBXGLEngine", (ok, info, src)
# the package hook is the same engine the bake installs
hooks = {f: dict((t, s) for s, t, _ in spec.get("hooks", [])) for f, spec in mkpkg.FAMILIES.items()}
assert hooks["n72-ios30"][mkpkg.MBX] == "contrib/it-gles/MBXGLEngine-30"
assert hooks["n72-ios3"][mkpkg.MBX] == "contrib/it-gles/MBXGLEngine"
# built: it must be legacy-linked and signed (3.0 kills a signed process at an unsigned library's first page)
built = os.path.join(ROOT, "contrib/it-gles/MBXGLEngine-30")
if os.path.exists(built):
    assert mkpkg.macho_problem(open(built, "rb").read(), "armv6", legacy=True, signed=True) is None
print("PASS: gli_engine per volume (2.x none, 3.0 MBXGLEngine-30, 3.1+ MBXGLEngine), package hooks agree%s"
      % (", MBXGLEngine-30 legacy and signed" if os.path.exists(built) else ""))
