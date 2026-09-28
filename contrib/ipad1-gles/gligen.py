#!/usr/bin/env python3
"""Generate gli_fwd.h for glishim.c from a firmware's docs/ipad1/gli-dispatch-<BUILD>.tsv.

One entry per dispatch slot (826 on 3.2.x, 841 on 4.2.1). A slot OpenGLES exports a trampoline
for, or that the real GLEngine fills for ES1 or ES2, gets a forwarder that
sends its arguments to the host under the 3.1.3 WIRE slot number (the TSV's
slot_3.1.3 column; the host decoder keys on those). Every other slot, and any
slot with no 3.1.3 equivalent, gets a stub that logs once and returns 0 -- the
trampolines never null-check. gli_slot313[] maps every slot to its 3.1.3 slot
(-1 if none), which is where glishim finds mbxshim's hand-written thunks.

Argument counts come from the TSV's macOS prototype column below the ES tail
(from alpha_funcx on) and from the table below for the tail, which has no
macOS prototype.

    gligen.py [--tsv TSV] OUT.h     generate (default TSV: 7B500's)
    gligen.py [--tsv TSV] --check   self-check only
"""
import os
import re
import sys

TSV = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                   "../../docs/ipad1/gli-dispatch-7B500.tsv")
if sys.argv[1:2] == ["--tsv"]:
    TSV, sys.argv[1:3] = sys.argv[2], []
N_SLOTS = sum(1 for line in open(TSV) if line[:1].isdigit())
TAIL = next(int(line.split("\t")[0]) for line in open(TSV)
            if line[:1].isdigit() and line.split("\t")[3] == "alpha_funcx")   # first ES/OES tail slot

# gl.h / glext.h argument counts (GC excluded) for the ES tail (7B500: slots 764-825).
TAIL_ARGC = {
    "glAlphaFuncx": 2, "glClearColorx": 4, "glClearDepthf": 1, "glClearDepthx": 1,
    "glClipPlanef": 2, "glClipPlanex": 2, "glColor4x": 4, "glDepthRangef": 2,
    "glDepthRangex": 2, "glFogx": 2, "glFogxv": 2, "glFrustumf": 6, "glFrustumx": 6,
    "glGetClipPlanef": 2, "glGetClipPlanex": 2, "glGetFixedv": 2,
    "glGetLightxv": 3, "glGetMaterialxv": 3, "glGetTexEnvxv": 3,
    "glGetTexParameterxv": 3, "glLightModelx": 2, "glLightModelxv": 2,
    "glLightx": 3, "glLightxv": 3, "glLineWidthx": 1, "glLoadMatrixx": 1,
    "glMaterialx": 3, "glMaterialxv": 3, "glMultMatrixx": 1, "glNormal3x": 3,
    "glOrthof": 6, "glOrthox": 6, "glPointSizex": 1, "glPolygonOffsetx": 2,
    "glRotatex": 4, "glScalex": 3, "glTexEnvx": 3, "glTexEnvxv": 3,
    "glTexParameterx": 3, "glTexParameterxv": 3, "glTranslatex": 3,
    "glMultiTexCoord4x": 5, "glSampleCoveragex": 2, "glPointParameterx": 2,
    "glPointParameterxv": 2, "glPointSizePointerOES": 3,
    "glCurrentPaletteMatrixOES": 1, "glLoadPaletteFromModelViewMatrixOES": 0,
    "glMatrixIndexPointerOES": 4, "glWeightPointerOES": 4,
    "glDrawTexsOES": 5, "glDrawTexiOES": 5, "glDrawTexxOES": 5, "glDrawTexfOES": 5,
    "glDrawTexsvOES": 1, "glDrawTexivOES": 1, "glDrawTexxvOES": 1, "glDrawTexfvOES": 1,
    "glShaderBinary": 5, "glGetShaderPrecisionFormat": 4,
    "glReleaseShaderCompiler": 0, "glFramebufferParameteriAPPLE": 3,
}


def batchable(name, proto, slot):
    """A call the guest cannot tell apart from a deferred one: returns nothing, passes the host no
    guest pointer, and does not draw (draws read client arrays from guest memory when they run) or sync."""
    base = name.rstrip("*")
    if "Draw" in base or base in ("glFlush", "glFinish") or "Fence" in base or "Get" in base or "Is" in base[:4]:
        return False
    if slot < TAIL:
        return proto.startswith("void (") and "*" not in proto
    # ES tail: no macOS prototype. The vector (…v, …vOES), pointer and matrix forms take pointers.
    return not (base.endswith("v") or base.endswith("vOES") or "Pointer" in base or "Matrix" in base
                or "ClipPlane" in base or base in ("glShaderBinary", "glReleaseShaderCompiler"))


def load():
    """-> list of (slot, name, wire or None, argc or None) for every slot."""
    rows = []
    SLOT313.clear()
    for line in open(TSV):
        if line.startswith(("#", "slot\t")):
            continue
        f = line.rstrip("\n").split("\t")
        slot, name = int(f[0]), f[4]
        wanted = f[5] == "Y" or f[6] == "Y" or f[7] == "Y"
        wire = int(f[8]) if f[8].isdigit() else None
        SLOT313.append(wire if wire is not None else -1)
        proto = f[9] if len(f) > 9 else ""
        argc = None
        if slot >= TAIL:
            argc = TAIL_ARGC.get(name)
        elif proto and "double" not in proto and "clampd" not in proto:
            # "void (GLIContext, a, b)" -> 2
            argc = proto[proto.index("(") + 1:proto.rindex(")")].count(",")
        rows.append((slot, name, wire if wanted else None, argc))
        BATCH[slot] = batchable(name, proto, slot)
    return rows


BATCH = {}
SLOT313 = []


def check(rows):
    assert len(rows) == N_SLOTS and [r[0] for r in rows] == list(range(N_SLOTS))
    by = {r[1].rstrip("*"): r for r in rows}
    # Known ES1 wire numbers the host already decodes (gles.h).
    assert by["glOrthof"][2] == 791 and by["glOrthof"][3] == 6
    assert by["glAlphaFuncx"][2] == 761 and by["glClearDepthf"][2] == 763
    assert by["glTexImage2D"][2] == 301 and by["glTexImage2D"][3] == 9
    assert by["glDrawArrays"][3] == 3 and by["glClear"][3] == 1
    # ES2 core sits below 761 with identical numbering.
    assert by["glUseProgram"][2] == 600 and by["glUniformMatrix2fv"][3] == 4
    assert by["glVertexAttribPointer"][2] is not None
    # New in 3.2: no wire number, so never forwarded.
    assert by["glFramebufferParameteriAPPLE"][2] is None
    for n in ("glEnable", "glBlendFunc", "glBindTexture", "glUniform4f", "glViewport", "glTranslatef",
              "glClearColor", "glUseProgram"):
        assert BATCH[by[n][0]], n
    for n in ("glDrawArrays", "glDrawElements", "glFlush", "glFinish", "glGetError", "glTexImage2D",
              "glUniform4fv", "glVertexPointer", "glReadPixels", "glIsEnabled", "glLoadMatrixx",
              "glGenTextures", "glCreateShader"):
        assert not BATCH[by[n][0]], n
    for slot, name, wire, argc in rows:
        if wire is not None:
            assert argc is not None, "no argc for forwarded %s (%d)" % (name, slot)
    # the 3.1.3 slots only move up, and never twice to one place
    s313 = [w for w in SLOT313 if w >= 0]
    assert s313 == sorted(s313) and len(set(s313)) == len(s313)
    if N_SLOTS == 826:   # 3.2.x: three slots inserted at 761, the rest +3
        assert all(w == (i if i < 761 else i - 3) for i, w in enumerate(SLOT313) if w >= 0)


def coverage(rows):
    """Markdown table: every ES1/ES2 entry point OpenGLES 3.2.2 exports, and
    what happens to it -- executed by the host, forwarded but unhandled there,
    or a guest stub. Host support is read out of hw/arm/gles-host.c."""
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "../..")
    hdr = open(os.path.join(root, "include/hw/arm/guest-services/gles.h")).read()
    host = open(os.path.join(root, "hw/arm/gles-host.c")).read()
    slots = dict(re.findall(r"#define (GLES_SLOT_\w+)\s+(\d+)", hdr))
    handled = {int(slots[m]) for m in re.findall(r"case (GLES_SLOT_\w+)", host) if m in slots}
    es2 = host[host.index("static bool gles_es2_call("):]
    es2 = es2[:es2.index("\n}\n")]
    for a, b in re.findall(r"case (\d+)(?: \.\.\. (\d+))?:", es2):
        handled.update(range(int(a), int(b or a) + 1))
    es = {}
    for line in open(TSV):
        if line.startswith(("#", "slot\t")):
            continue
        f = line.rstrip("\n").split("\t")
        if f[7] == "Y":
            es[int(f[0])] = (f[4], f[5] == "Y", f[6] == "Y")
    out = ["| slot | entry point | ES1 | ES2 | status |", "|---|---|---|---|---|"]
    counts = {}
    for slot, name, wire, argc in rows:
        if slot not in es:
            continue
        name, e1, e2 = es[slot]
        if wire is None:
            st = "stub (no wire slot)"
        elif name == "glGetString":
            st = "guest (glishim answers)"
        elif wire in handled:
            st = "host"
        else:
            st = "forwarded, host UNHANDLED"
        counts[st] = counts.get(st, 0) + 1
        out.append("| %d | %s | %s | %s | %s |" % (slot, name, "Y" if e1 else "", "Y" if e2 else "", st))
    summary = ", ".join("%s: %d" % kv for kv in sorted(counts.items()))
    return summary, "\n".join(out)


rows = load()
check(rows)
if sys.argv[1:] == ["--coverage"]:
    summary, table = coverage(rows)
    print(summary)
    print()
    print(table)
    sys.exit(0)
if sys.argv[1:] == ["--check"]:
    print("gligen self-check OK: %d slots, %d forwarded"
          % (len(rows), sum(r[2] is not None for r in rows)))
    sys.exit(0)

out = ["/* Generated by gligen.py from %s -- do not edit. */" % os.path.basename(TSV),
       "#define GLI_N_SLOTS %d" % N_SLOTS,
       "static int gli_unimpl(unsigned slot);",
       "/* Function name per slot, for the unimplemented-slot report (* = name",
       " * derived from the dispatch field, no OpenGLES export). */",
       "static const char *const gli_slot_names[GLI_N_SLOTS] = {"]
out += ['    "%s",' % name for slot, name, wire, argc in load()]
out += ["};", ""]
out.append("/* 3.1.3 (mbxshim, wire) slot per slot, -1 = none. */")
out.append("static const short gli_slot313[GLI_N_SLOTS] = {")
for i in range(0, N_SLOTS, 16):
    out.append("    %s," % ", ".join(str(w) for w in SLOT313[i:i + 16]))
out += ["};", ""]
for slot, name, wire, argc in rows:
    if wire is None:
        out.append("static int g%d(void *gc) { (void)gc; return gli_unimpl(%d); }"
                   % (slot, slot))
        continue
    params = "".join(", unsigned a%d" % i for i in range(argc))
    args = ", ".join("a%d" % i for i in range(argc)) or "0"
    out.append("static int g%d(void *gc%s) /* %s */ { return (int)qc(%d, gc, %d, A(%s)); }"
               % (slot, params, name, wire, argc, args))
out.append("")
# Slot numbers by name, so glishim.c's overrides cannot name the wrong slot.
for slot, name, wire, argc in rows:
    if wire is not None:
        out.append("#define GLI_SLOT_%s %d" % (name, slot))
out.append("")
wires = {wire: slot for slot, name, wire, argc in rows if wire is not None}
top = max(wires) + 1
out.append("/* By WIRE number: calls glishim may queue (see gles_batch). */")
out.append("static const unsigned char gli_batchable[%d] = {" % top)
for i in range(0, top, 32):
    out.append("    %s," % ",".join("1" if i + j in wires and BATCH[wires[i + j]] else "0"
                                    for j in range(min(32, top - i))))
out.append("};")
out.append("")
out.append("static void *const gli_fwd_table[GLI_N_SLOTS] = {")
for i in range(0, N_SLOTS, 8):
    out.append("    %s," % ", ".join("(void *)g%d" % j for j in range(i, min(i + 8, N_SLOTS))))
out.append("};")
open(sys.argv[1], "w").write("\n".join(out) + "\n")
print("wrote %s" % sys.argv[1])
