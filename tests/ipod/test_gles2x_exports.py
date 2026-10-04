#!/usr/bin/env python3
"""The 1.x and 2.x OpenGLES export lists build: every gl* name has a gles-names.h row (or its OES
spelling's, or a hand thunk); 1.x uses gles1x.c and 2.x uses the shared public front end."""
import os, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "contrib/it-gles"))
import gles2x_exports as g

NAMES = os.path.join(ROOT, "include/hw/arm/guest-services/gles-names.h")
for os_, count in (("1x", 186), ("2x", 218)):
    listfile = os.path.join(ROOT, "contrib/it-gles/opengles-%s.exports" % os_)
    names = g.read_list(listfile)
    assert len(names) == count, (os_, len(names))
    with tempfile.TemporaryDirectory() as out:
        g.gen(listfile, NAMES, out)
        exp = open(os.path.join(out, "gles2x.exp")).read().split()
        assert exp == ["_" + n for n in names], os_
# 1.x only: the two names 2.x dropped, and none of EAGL (1.x has no EAGL: its ObjC is the old ABI)
one = set(g.read_list(os.path.join(ROOT, "contrib/it-gles/opengles-1x.exports")))
assert {"eglSwapNotification", "glVertexAttribPointerARB"} <= one
assert not any("EAGL" in n for n in one)
# The one front end's list (contrib/gles-public): 5.0 beta 1 (9A5220p) exports the ...EXT set as ...APPLE, and each
# forwards to its ...EXT twin's row (the same host dispatch)
listfile = os.path.join(ROOT, "contrib/gles-public/opengles.exports")
names = g.read_list(listfile)
with tempfile.TemporaryDirectory() as out:
    g.gen(listfile, NAMES, out)
    fwd = dict(l[len("GLES2X_FWD("):-1].split(", ") for l in open(os.path.join(out, "gles2x_exports.h")) if l.startswith("GLES2X_FWD("))
twins = [n for n in names if n.endswith("APPLE") and n[:-5] + "EXT" in names]
assert len(twins) == 43 and "glBeginQueryAPPLE" in twins and "glActiveShaderProgramAPPLE" in twins, len(twins)
for n in twins:
    assert fwd[n] == fwd[n[:-5] + "EXT"], (n, fwd[n], fwd[n[:-5] + "EXT"])
have = g.rows(NAMES)
assert g.row_of("glNoSuchCallAPPLE", have) is None and g.row_of("glRenderbufferStorageMultisampleAPPLE", have) == "glRenderbufferStorageMultisampleAPPLE"
print("PASS: opengles-1x (186) and opengles-2x (218) export lists generate; the front end's %d (%d ...APPLE on their ...EXT rows)"
      % (len(names), len(twins)))
