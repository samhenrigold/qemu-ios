#!/usr/bin/env python3
"""Derive a firmware's GLI dispatch TSV (docs/ipad1/gli-dispatch-<BUILD>.tsv) from its shared cache.

    glitsv.py CACHE BUILD OUT.tsv [--base docs/ipad1/gli-dispatch-7B500.tsv]
    glitsv.py --verify CACHE TSV    the TSV's fields and export column are this cache's

From the firmware: the slot list (the ObjC @encode of __GLIFunctionDispatchRec in OpenGLES, as
ipad1_rootfs.gli_abi_problem reads it) and which slots OpenGLES exports a trampoline for (each _gl*
export loads its target with one `ldr rX, [rY, #off]`, slot = (off - 0x10) / 4). Carried from the base
table by dispatch-field name, since they are facts about the function rather than the slot: the
GLEngine ES1/ES2 fills, the 3.1.3 (wire) slot and the macOS prototype. A field the base table lacks
takes its wire slot from the 3.1.3 layout itself (docs/ipod/gli-dispatch-7E18.tsv, the numbering the host
decodes), else it gets none and gligen.py stubs it. The iPod's tables come from its armv6 caches the same way.
"""
import argparse, os, re, struct, sys
import capstone

HERE = os.path.dirname(os.path.abspath(__file__))
BASE = os.path.join(HERE, "../../docs/ipad1/gli-dispatch-7B500.tsv")
WIRE = os.path.join(HERE, "../../docs/ipod/gli-dispatch-7E18.tsv")    # 3.1.3: slot = wire number
OPENGLES = b"/System/Library/Frameworks/OpenGLES.framework/OpenGLES"
TABLE, GC_OFF, TSD_OFF = 0x10, 0xC, 0xC0     # as contrib/it-gles/genstubs.py


def fields(cache):
    enc = re.search(rb"\{__GLIFunctionDispatchRec=[^}]*\}", cache)
    return [f.decode() for f in re.findall(rb'"([^"]+)"', enc[0])]


def exports(cache, nslots):
    """{slot: _gl* export name} for OpenGLES in a dyld_v1 shared cache."""
    assert cache[:7] == b"dyld_v1"
    moff, mcount, ioff, icount = struct.unpack_from("<4I", cache, 0x10)
    maps = [struct.unpack_from("<QQQ", cache, moff + i * 32) for i in range(mcount)]
    f = lambda va: next(fo + va - a for a, sz, fo in maps if a <= va < a + sz)
    for i in range(icount):
        va, _, _, path = struct.unpack_from("<QQQI", cache, ioff + i * 32)
        if cache[path:cache.index(b"\0", path)] == OPENGLES:
            break
    else:
        raise SystemExit("no OpenGLES image in the cache")
    mh = f(va)
    ncmds, off = struct.unpack_from("<I", cache, mh + 16)[0], mh + 28
    for _ in range(ncmds):
        cmd, size = struct.unpack_from("<II", cache, off)
        if cmd == 2:                                          # LC_SYMTAB: cache file offsets
            symoff, nsyms, stroff = struct.unpack_from("<III", cache, off + 8)
        off += size
    arm, thumb = (capstone.Cs(capstone.CS_ARCH_ARM, m) for m in (capstone.CS_MODE_ARM, capstone.CS_MODE_THUMB))
    out = {}
    for k in range(nsyms):
        strx, typ, _, desc, value = struct.unpack_from("<IBBhI", cache, symoff + 12 * k)
        name = cache[stroff + strx:cache.index(b"\0", stroff + strx)].decode()
        if not name.startswith("_gl") or typ & 0x0F != 0x0F:   # N_SECT | N_EXT
            continue
        cs = thumb if desc & 8 else arm                     # N_ARM_THUMB_DEF
        # ... ldr rN, [rT, #off]; blx rN ... pop {..., pc}: the slot is the load the call goes through.
        # armv6 tail-calls it (... bx rN) after a conditional early return (popeq {..., pc} / bxeq lr),
        # so only an unconditional pop {..., pc} or bx ends the trampoline. 3.x armv6 also loads the
        # slot straight into pc: `mov lr, pc; ldr pc, [ip, #off]` (a call), or a bare one (a tail call).
        loads, offs, prev = {}, set(), ""
        for ins in cs.disasm(cache[f(value):f(value) + 160], value):
            m = re.match(r"(\w+), \[(?:r\d+|ip), #(0x[0-9a-f]+)\]", ins.op_str)
            if ins.mnemonic.startswith("ldr") and m:
                loads[m.group(1)] = int(m.group(2), 16)
                if m.group(1) == "pc":
                    offs.add(loads["pc"])
                    if ins.mnemonic == "ldr" and prev != "mov lr, pc":
                        break
            elif ins.mnemonic[:2] in ("bl", "bx") and ins.op_str in loads:   # blx, bx, blxne (4.x glIs*)
                offs.add(loads[ins.op_str])
            if (ins.mnemonic == "pop" and "pc" in ins.op_str) or ins.mnemonic == "bx":
                break
            prev = "%s %s" % (ins.mnemonic, ins.op_str)
        offs = {o for o in offs if o not in (GC_OFF, TSD_OFF) and TABLE <= o < TABLE + 4 * nslots}
        if len(offs) == 1:
            out[(offs.pop() - TABLE) // 4] = name[1:]
        elif offs:
            print("skip %s: ambiguous %s" % (name, sorted(map(hex, offs))), file=sys.stderr)
    return out


def rows(path):
    return [l.rstrip("\n").split("\t") for l in open(path) if l[:1].isdigit()]


def derive(cache, build, base):
    fl, base_rows = fields(cache), rows(base)
    ex = exports(cache, len(fl))
    by = {r[3]: r for r in base_rows}
    wire = {r[3]: r[0] for r in rows(WIRE)}
    out = []
    for slot, field in enumerate(fl):
        b = by.get(field)
        name = ex.get(slot) or (b[4] if b else "gl" + "".join(w[:1].upper() + w[1:] for w in field.split("_")) + "*")
        es1, es2, w313, proto = (b[5], b[6], b[8], b[9] if len(b) > 9 else "") if b else ("", "", wire.get(field, "-"), "")
        out.append([str(slot), "0x%03x" % (4 * slot), "0x%03x" % (4 * slot + TABLE), field, name,
                    es1, es2, "Y" if slot in ex else "", w313, proto])
    hdr = ["# __GLIFunctionDispatchRec for %s, %d slots (0x%X bytes), generated by contrib/gles-public/glitsv.py." % (build, len(fl), 4 * len(fl) + TABLE),
           "# From this firmware: slot/dispatch_field (@encode in OpenGLES) and OpenGLES_export (trampolines). Carried from",
           "# %s by dispatch_field: gl_function (unless exported), es1/es2_filled, slot_3.1.3, prototype; - = no wire slot." % os.path.basename(base),
           "slot\tbyte_off\teagl_ctx_off\tdispatch_field\tgl_function\tes1_filled\tes2_filled\tOpenGLES_export\tslot_3.1.3\tmacOS_gliDispatch_prototype"]
    return "\n".join(hdr + ["\t".join(r) for r in out]) + "\n", ex


def verify(cache, tsv):
    fl, ex, rs = fields(cache), exports(cache, len(fields(cache))), rows(tsv)
    assert [r[3] for r in rs] == fl, "dispatch fields differ"
    want = {int(r[0]) for r in rs if r[7] == "Y"}
    assert set(ex) == want, "export column differs: +%s -%s" % (sorted(set(ex) - want), sorted(want - set(ex)))
    print("%s matches: %d slots, %d exports" % (os.path.basename(tsv), len(fl), len(ex)))


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--verify", action="store_true")
    ap.add_argument("--base", default=BASE)
    ap.add_argument("cache")
    ap.add_argument("build_or_tsv")
    ap.add_argument("out", nargs="?")
    a = ap.parse_args()
    data = open(a.cache, "rb").read()
    if a.verify:
        verify(data, a.build_or_tsv)
    else:
        text, ex = derive(data, a.build_or_tsv, a.base)
        open(a.out, "w").write(text)
        print("wrote %s: %d exports" % (a.out, len(ex)))
