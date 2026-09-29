#!/usr/bin/env python3
"""The 1.x and 2.x OpenGLES export lists build: every gl* name has a gles-names.h row (or its OES
spelling's, or a hand thunk in gles2x.c), so contrib/it-gles/build-gles2x.sh 1x|2x cannot fail on a gap."""
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
print("PASS: opengles-1x (186) and opengles-2x (218) export lists generate")
