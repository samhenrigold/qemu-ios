#!/usr/bin/env python3
"""The GL bridge's name table: include/hw/arm/guest-services/gles-names.h, one GLES_FN row per GL function.

    gligen.py --check                 the header is consistent (ids, argc, flags, its stamp, the host's raw ids)
    gligen.py --stamp                 rewrite the header's GLES_NAMES_VERSION after a hand edit
    gligen.py --from-tsvs OUT.h       regenerate the whole table from docs/*/gli-dispatch-*.tsv (how it was first
                                      made; a row-by-row diff against the committed header is the review)

Both the guest shims (contrib/it-gles/mbxshim.c, contrib/ipad1-gles/glishim.c) and the host (gles.h,
gles-host.c) include the header, so the wire ids cannot drift between them. A row is
GLES_FN(name, dispatch_field, id, argc, flags): the function's exported name, its field in OpenGLES's
__GLIFunctionDispatchRec (what the shim discovers at load), the wire id, how many 32-bit arguments it
carries after the GC (NA: not forwardable, the shim stubs it), and GLES_F_BATCH / GLES_F_EXPORT. Ids are
append-only: the ones below 822 are the 3.1.3 dispatch slots the host has always decoded, the rest were
assigned from 822 up. A new function is a new row with a new id, never a reused one.
"""
import glob
import os
import re
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "../..")
HEADER = os.path.join(ROOT, "include/hw/arm/guest-services/gles-names.h")
WIRE_TSV = os.path.join(ROOT, "docs/ipod/gli-dispatch-7E18.tsv")     # 3.1.3: slot = id
GLES_OP_BASE = 0x1000
F_BATCH, F_EXPORT = 1, 2

# gl.h / glext.h argument counts (GC excluded) for the ES/OES tail, which has no macOS prototype.
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
    # 4.2.1's additions (glext.h); glDiscardFramebufferEXT stays a guest no-op (mbxshim inert_stub)
    "glBindVertexArrayOES": 1, "glDeleteVertexArraysOES": 2, "glGenVertexArraysOES": 2, "glIsVertexArrayOES": 1,
    "glRenderbufferStorageMultisampleAPPLE": 5, "glResolveMultisampleFramebufferAPPLE": 0,
}


def batchable(name, proto):
    """A call the guest cannot tell apart from a deferred one: returns nothing, passes the host no
    guest pointer, and does not draw (draws read client arrays from guest memory when they run) or sync."""
    base = name.rstrip("*")
    if "Draw" in base or base in ("glFlush", "glFinish") or "Fence" in base or "Get" in base or "Is" in base[:4]:
        return False
    if proto:
        return proto.startswith("void (") and "*" not in proto
    # ES tail: no macOS prototype. The vector (…v, …vOES), pointer, matrix and name-array
    # (glGen*/glDelete*) forms take pointers.
    return not (base.endswith("v") or base.endswith("vOES") or "Pointer" in base or "Matrix" in base
                or "ClipPlane" in base or "Gen" in base or "Delete" in base
                or base in ("glShaderBinary", "glReleaseShaderCompiler"))


def canonical(names):
    """One name per field: an exported one, and the plain spelling over the OES/EXT alias
    (the host implements glBindFramebuffer; 3.x exports it as glBindFramebufferOES)."""
    real = sorted(n for n in names if not n.endswith("*")) or sorted(names)
    return min(real, key=lambda n: (n.endswith(("OES", "EXT", "APPLE", "ARB")), n))


def from_tsvs():
    """[(name, field, id, argc, flags)] from the union of docs/*/gli-dispatch-*.tsv, in first-seen order."""
    rows, order, wire = {}, [], {}
    for line in open(WIRE_TSV):
        if line[:1].isdigit():
            f = line.rstrip("\n").split("\t")
            wire[f[3]] = int(f[0])
    for tsv in [os.path.join(ROOT, "docs/ipad1/gli-dispatch-7B500.tsv")] + sorted(glob.glob(os.path.join(ROOT, "docs/*/gli-dispatch-*.tsv"))):
        for line in open(tsv):
            if not line[:1].isdigit():
                continue
            f = line.rstrip("\n").split("\t")
            field, name, exported = f[3], f[4], f[7] == "Y"
            proto = f[9] if len(f) > 9 else ""
            r = rows.get(field)
            if r is None:
                r = rows[field] = {"names": set(), "exported": False, "proto": proto}
                order.append(field)
            r["names"].add(name)
            r["exported"] |= exported
    out, next_id = [], 822
    for field in order:
        r = rows[field]
        name, proto = canonical(r["names"]), r["proto"]
        if field in wire:
            id_ = wire[field]
        else:
            id_, next_id = next_id, next_id + 1
        if proto and "double" not in proto and "clampd" not in proto:
            argc = proto[proto.index("(") + 1:proto.rindex(")")].count(",")   # "void (GLIContext, a, b)" -> 2
        else:
            argc = TAIL_ARGC.get(name)
        flags = (F_BATCH if argc is not None and batchable(name, proto) else 0) | (F_EXPORT if r["exported"] else 0)
        out.append((name.rstrip("*"), field, id_, argc, flags))
    assert sorted(id_ for _, _, id_, _, _ in out if id_ < 822) == sorted(wire.values())
    return out


ROW = re.compile(r"^GLES_FN\((\w+),\s*(\w+),\s*(\d+),\s*(\d+|NA),\s*([\w|]+)\)")


def load(path=HEADER):
    """(rows as from_tsvs gives them, the stamped version or None, the file's text)."""
    rows, version, text = [], None, open(path).read()
    for line in text.split("\n"):
        m = ROW.match(line)
        if m:
            name, field, id_, argc, flags = m.groups()
            fl = sum({"GLES_F_BATCH": F_BATCH, "GLES_F_EXPORT": F_EXPORT, "0": 0}[t] for t in flags.split("|"))
            rows.append((name, field, int(id_), None if argc == "NA" else int(argc), fl))
        m = re.match(r"#define GLES_NAMES_VERSION 0x([0-9a-f]+)", line)
        if m:
            version = int(m.group(1), 16)
    return rows, version, text


def stamp(rows):
    return zlib.crc32("\n".join("%s %s %d %s %d" % r for r in rows).encode()) & 0xffffffff


def flag_text(fl):
    return "|".join(n for b, n in ((F_BATCH, "GLES_F_BATCH"), (F_EXPORT, "GLES_F_EXPORT")) if fl & b) or "0"


def render(rows):
    w = max(len(r[0]) for r in rows) + 1
    body = ["GLES_FN(%-*s %-45s %4d, %-3s %s)" % (w, r[0] + ",", r[1] + ",", r[2], ("NA" if r[3] is None else str(r[3])) + ",", flag_text(r[4]))
            for r in rows]
    return """/*
 * The GL bridge's function table: one GLES_FN(name, dispatch_field, id, argc, flags) per GL function,
 * shared by the guest shims (contrib/it-gles/mbxshim.c, contrib/ipad1-gles/glishim.c) and the host
 * (gles.h, gles-host.c), so the wire cannot drift between them. Included more than once with
 * GLES_FN defined for the occasion (an X-macro list), so the rows have no include guard.
 *
 *   name            the exported gl* name (the plain spelling where 3.x exports an OES alias)
 *   dispatch_field  its field in OpenGLES's __GLIFunctionDispatchRec: the shim reads the
 *                   firmware's @encode of that struct at load and finds the slot by this name
 *   id              the wire id, stable across firmwares. Below 822 it is the 3.1.3 dispatch
 *                   slot the host has always decoded; from 822 up, assigned here. Append-only:
 *                   a new function is a new row with a new id, never a reused one
 *   argc            32-bit arguments after the GC (floats as bit patterns); NA = the shim
 *                   cannot forward it (a double, or no known prototype) and stubs it by name
 *   flags           GLES_F_BATCH: returns nothing, no guest pointer, no draw or sync, so glishim
 *                   may queue it (GLES_OP_BATCH); GLES_F_EXPORT: some firmware's OpenGLES
 *                   exports a trampoline for it (the discovery fallback tries only these)
 *
 * Generated once by contrib/ipad1-gles/gligen.py --from-tsvs from the 7E18, 7B500 and 8C148
 * dispatch tables (docs/ipod, docs/ipad1 gli-dispatch-*.tsv); hand-edited since. After an edit
 * run gligen.py --stamp (GLES_NAMES_VERSION is the rows' CRC, carried in the shim's hello) and
 * --check.
 */
#ifndef GLES_NAMES_VERSION
#define GLES_NAMES_VERSION 0x%08x
#define GLES_ID_MAX %d              /* ids are 0..GLES_ID_MAX, all below GLES_OP_BASE */
#define GLES_F_BATCH 1
#define GLES_F_EXPORT 2
#endif
#ifdef GLES_FN
%s
#endif
""" % (stamp(rows), max(r[2] for r in rows), "\n".join(body))


def check(rows, version):
    names, fields, ids = [r[0] for r in rows], [r[1] for r in rows], [r[2] for r in rows]
    assert len(set(names)) == len(names), "duplicate name"
    assert len(set(fields)) == len(fields), "duplicate dispatch field"
    assert len(set(ids)) == len(ids) and max(ids) < GLES_OP_BASE, "duplicate or out-of-range id"
    assert version == stamp(rows), "GLES_NAMES_VERSION is stale: run gligen.py --stamp"
    by = {r[0]: r for r in rows}
    # Known ES1 wire numbers the host has always decoded (gles.h).
    assert by["glOrthof"][2] == 791 and by["glOrthof"][3] == 6
    assert by["glAlphaFuncx"][2] == 761 and by["glClearDepthf"][2] == 763
    assert by["glTexImage2D"][2] == 301 and by["glTexImage2D"][3] == 9
    assert by["glDrawArrays"][3] == 3 and by["glClear"][3] == 1
    assert by["glUseProgram"][2] == 600 and by["glUniformMatrix2fv"][3] == 4
    assert by["glVertexAttribPointer"][3] == 6 and by["glBindFramebuffer"][2] == 672
    for n in ("glEnable", "glBlendFunc", "glBindTexture", "glUniform4f", "glViewport", "glTranslatef",
              "glClearColor", "glUseProgram"):
        assert by[n][4] & F_BATCH, n
    for n in ("glDrawArrays", "glDrawElements", "glFlush", "glFinish", "glGetError", "glTexImage2D",
              "glUniform4fv", "glVertexPointer", "glReadPixels", "glIsEnabled", "glLoadMatrixx",
              "glGenTextures", "glCreateShader"):
        assert not by[n][4] & F_BATCH, n
    for name, field, id_, argc, flags in rows:
        assert argc is None or 0 <= argc <= 12, name
        assert not (flags & F_BATCH) or argc is not None, name
    # Every raw id the host's ES2 switch and the shim's hand thunks name is a row's.
    raw = set()
    if os.path.exists(os.path.join(ROOT, "hw/arm/gles-host.c")):   # absent in a guest-tools source copy
        host = open(os.path.join(ROOT, "hw/arm/gles-host.c")).read()
        es2 = host[host.index("static bool gles_es2_call("):]
        es2 = es2[:es2.index("\n}\n")]
        for a, b in re.findall(r"case (\d+)(?: \.\.\. (\d+))?:", es2):
            raw.update(range(int(a), int(b or a) + 1))
    shim = open(os.path.join(ROOT, "contrib/it-gles/mbxshim.c")).read()
    raw.update(int(x) for x in re.findall(r"\bqc\((\d+),", shim))
    lost = sorted(raw - set(ids))
    assert not lost, "ids used by gles-host.c / mbxshim.c that are not in the table: %s" % lost
    # the 3.1.3 trampoline map (tests read it) agrees with the ids
    for slot, name in re.findall(r"^(\d+) (\w+)$", open(os.path.join(ROOT, "contrib/it-gles/slotmap.txt")).read(), re.M):
        # 3.1.3 exports the OES spelling, and glDeleteProgram/glDeleteShader share one slot
        r = by.get(name) or by.get(re.sub(r"(OES|EXT|APPLE)$", "", name))
        assert int(slot) in ids and (r is None or r[2] == int(slot)), "%s: slotmap says %s" % (name, slot)


if __name__ == "__main__":
    a = sys.argv[1:]
    if a[:1] == ["--from-tsvs"] and len(a) == 2:
        rows = from_tsvs()
        check(rows, stamp(rows))
        open(a[1], "w").write(render(rows))
        print("wrote %s: %d functions, %d forwardable, %d batchable" % (
            a[1], len(rows), sum(r[3] is not None for r in rows), sum(bool(r[4] & F_BATCH) for r in rows)))
    elif a == ["--stamp"]:
        rows, version, text = load()
        open(HEADER, "w").write(re.sub(r"#define GLES_NAMES_VERSION 0x[0-9a-f]+", "#define GLES_NAMES_VERSION 0x%08x" % stamp(rows), text))
        print("stamped %08x" % stamp(rows))
    elif a == ["--check"]:
        rows, version, _ = load()
        check(rows, version)
        print("gligen self-check OK: %d functions, %d forwardable, version %08x" % (
            len(rows), sum(r[3] is not None for r in rows), version))
    else:
        sys.exit(__doc__)
