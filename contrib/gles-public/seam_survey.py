#!/usr/bin/env python3
"""What every image of a firmware takes from OpenGLES.framework: the public seam's survey
(LightTouchMac docs/ipad1/gles-public-seam.md is its output, made by --markdown).

    seam_survey.py BUILD=PATH[+EXE...] ...   PATH: a dyld_v1 shared cache, or (1.x/2.x, no cache) a directory
                                       holding the framework binaries by name (OpenGLES, QuartzCore, UIKit, ...)
                                       and SpringBoard; +EXE: executables outside the cache (SpringBoard)
    seam_survey.py --json OUT.json BUILD=PATH ...
    seam_survey.py --markdown OUT.md --json IN.json

Per build: OpenGLES's exports; the EAGL classes' methods (its ObjC metadata); the selectors OpenGLES sends to
other objects (a CAEAGLLayer's nativeWindow, drawableProperties); the private structures by @encode
(_EAGLNativeWindowObject, EAGLNativeWindowCallbacksRec, __GLIFunctionDispatchRec); and for every image that links
OpenGLES (SpringBoard too), the OpenGLES symbols it imports (by two-level ordinal) and the EAGL selectors it
sends. Read-only, no device.
"""
import json, os, re, struct, sys

OPENGLES = "/System/Library/Frameworks/OpenGLES.framework/OpenGLES"
LINK_CMDS = (0xC, 0x80000018, 0x8000001F, 0x80000023, 0x20)     # load, weak, reexport, upward, lazy
NSOBJECT_SELS = {"init", "dealloc", "release", "retain", "autorelease", "alloc", "class", "description", "hash",
                 "isEqual:", "retainCount", "finalize", "copy", "self", "zone"}


def cstr(d, o):
    return d[o:d.index(b"\0", o)].decode("latin1")


class Image:
    """A 32-bit ARM Mach-O at file offset `mh` of `d`; va2off maps its addresses (the cache's, or its own)."""

    def __init__(self, d, mh, va2off=None, path=""):
        self.d, self.mh, self.path = d, mh, path
        magic, cpu, sub, ftype, ncmds = struct.unpack_from("<5I", d, mh)
        assert magic == 0xFEEDFACE, path
        self.cmds, off = [], mh + 28
        for _ in range(ncmds):
            cmd, size = struct.unpack_from("<II", d, off)
            self.cmds.append((cmd, off))
            off += size
        self.segs, self.sects = [], {}
        for cmd, o in self.cmds:
            if cmd == 1:
                vm, vs, fo, fs = struct.unpack_from("<4I", d, o + 24)
                self.segs.append((vm, vs, fo))
                for k in range(struct.unpack_from("<I", d, o + 48)[0]):
                    so = o + 56 + 68 * k
                    name = d[so:so + 16].rstrip(b"\0").decode()
                    addr, size = struct.unpack_from("<II", d, so + 32)
                    self.sects[name] = (addr, size)
        self.va2off = va2off or (lambda va: next(mh + fo + va - vm for vm, vs, fo in self.segs if vm <= va < vm + vs))

    def dylibs(self):
        return [cstr(self.d, o + struct.unpack_from("<I", self.d, o + 8)[0]) for c, o in self.cmds if c in LINK_CMDS]

    def symbols(self):
        """(name, type, desc) from LC_SYMTAB; the offsets are file offsets (the cache's shared linkedit)."""
        for c, o in self.cmds:
            if c != 2:
                continue
            symoff, nsyms, stroff = struct.unpack_from("<III", self.d, o + 8)
            for k in range(nsyms):         # file offsets: the cache's shared linkedit, or a thin file's own
                sx, ty, _, desc = struct.unpack_from("<IBBH", self.d, symoff + 12 * k)
                yield cstr(self.d, stroff + sx), ty, desc

    def exports(self):
        return sorted({n for n, t, _ in self.symbols() if t & 0xE0 == 0 and t & 1 and t & 0xE not in (0, 0xC)})

    def imports_from(self, lib, exports):
        """Undefined externals bound to `lib` (two-level: by ordinal; flat, as 1.x/2.x frameworks are: any
        undefined name `lib` exports), and whether each is weak."""
        libs = self.dylibs()
        if lib not in libs:
            return {}
        want, flat = libs.index(lib) + 1, not struct.unpack_from("<I", self.d, self.mh + 24)[0] & 0x80
        out = {}
        for n, t, desc in self.symbols():
            if t & 0xE0 == 0 and t & 1 and t & 0xE in (0, 0xC) and n and (desc >> 8 == want or flat and n in exports):
                out[n] = bool(desc & 0x40)
        return out

    def u32(self, va):
        return struct.unpack_from("<I", self.d, self.va2off(va))[0]

    def str_at(self, va):
        return cstr(self.d, self.va2off(va))

    def section_words(self, name):
        if name not in self.sects:
            return []
        addr, size = self.sects[name]
        return [self.u32(addr + 4 * i) for i in range(size // 4)]

    def selrefs(self):
        return {self.str_at(p) for p in self.section_words("__objc_selrefs") if p}

    def methods(self, ml):
        if not ml:
            return []
        entsize, count = struct.unpack_from("<II", self.d, self.va2off(ml))
        entsize &= ~3
        return [self.str_at(self.u32(ml + 8 + i * entsize)) for i in range(count)]

    def classes(self):
        """{class name: (instance methods, class methods)} from __objc_classlist."""
        out = {}
        for cls in self.section_words("__objc_classlist"):
            isa, _, _, _, data = struct.unpack_from("<5I", self.d, self.va2off(cls))
            ro = data & ~3
            name, meths = struct.unpack_from("<II", self.d, self.va2off(ro) + 16)
            mdata = struct.unpack_from("<5I", self.d, self.va2off(isa))[4] & ~3
            cmeths = struct.unpack_from("<II", self.d, self.va2off(mdata) + 16)[1]
            out[self.str_at(name)] = (self.methods(meths), self.methods(cmeths))
        return out


class Firmware:
    """The images of one build: a dyld_v1 cache, or a directory of binaries (1.x/2.x)."""

    def __init__(self, path, extra=()):
        self.images, self.blob = {}, b""
        for x in extra:                                 # executables outside the cache (SpringBoard)
            img = self.load_file(x, "/System/Library/CoreServices/SpringBoard.app/" + os.path.basename(x))
            if img:
                self.images[img.path] = img
        if os.path.isfile(path):
            d = self.blob = open(path, "rb").read()
            assert d[:7] == b"dyld_v1", path
            moff, mcount, ioff, icount = struct.unpack_from("<4I", d, 0x10)
            maps = [struct.unpack_from("<QQQ", d, moff + 32 * i) for i in range(mcount)]

            def f(va):
                for a, sz, fo in maps:
                    if a <= va < a + sz:
                        return fo + va - a
                raise KeyError(hex(va))
            for i in range(icount):
                va, _, _, p = struct.unpack_from("<QQQI", d, ioff + 32 * i)
                img = Image(d, f(va), f, cstr(d, p))
                self.images[img.path] = img
            return
        for name in sorted(os.listdir(path)):
            img = self.load_file(os.path.join(path, name), "/System/Library/CoreServices/SpringBoard.app/" + name)
            if img:
                self.images[img.path] = img
                self.blob += img.d

    @staticmethod
    def load_file(p, default_path):
        """A thin or fat (armv7 slice, else armv6) Mach-O file as an Image named by its LC_ID_DYLIB."""
        d = open(p, "rb").read()
        if d[:4] == b"\xca\xfe\xba\xbe":
            slices = {}
            for i in range(struct.unpack_from(">I", d, 4)[0]):
                cpu, sub, off, size, _ = struct.unpack_from(">5I", d, 8 + 20 * i)
                if cpu == 12:
                    slices[sub] = d[off:off + size]
            d = slices.get(9) or slices.get(6) or b""
        if d[:4] != b"\xce\xfa\xed\xfe":
            return None
        img = Image(d, 0, None, p)
        ident = [cstr(d, o + struct.unpack_from("<I", d, o + 8)[0]) for c, o in img.cmds if c == 0xD]
        img.path = ident[0] if ident else default_path
        return img

    def encode(self, tag):
        """The fields of the first {tag=...} @encode anywhere in the build."""
        m = re.search(rb"\{" + re.escape(tag.encode()) + rb"=([^{}]*(?:\{[^{}]*\}[^{}]*)*)\}", self.blob)
        if not m:
            return None
        body = m.group(1).decode("latin1")
        return re.findall(r'"([^"]+)"', body) or ["(unnamed) " + body]


def kind(sym):
    """public Khronos / Apple-or-vendor extension / EAGL public / private"""
    n = sym.lstrip("_")
    if n.startswith("OBJC_CLASS_$_EAGL") or n.startswith("OBJC_METACLASS_$_EAGL"):
        return "EAGL class"
    if n.startswith("kEAGL"):
        return "EAGL constant"
    if n.startswith("egl"):
        return "EGL"
    if re.match(r"gl[A-Z]", n):
        return "gl extension" if re.search(r"(OES|EXT|APPLE|IMG|ARB|NV|AMD)$", n) else "gl core"
    if n in ("EAGLGetVersion",):
        return "EAGL function"
    return "private"


def survey(build, path):
    path, *extra = path.split("+")
    fw = Firmware(path, extra)
    ogl = fw.images.get(OPENGLES)
    rec = {"build": build, "source": os.path.basename(path), "images": len(fw.images)}
    if not ogl:
        rec["error"] = "no OpenGLES image"
        return rec
    rec["exports"] = ogl.exports()
    classes = ogl.classes()
    rec["classes"] = {c: {"instance": sorted(set(i)), "class": sorted(set(k))} for c, (i, k) in classes.items()}
    implemented = {s for i, k in classes.values() for s in i + k}
    rec["sends"] = sorted(ogl.selrefs() - implemented - NSOBJECT_SELS)
    rec["structs"] = {t: fw.encode(t) for t in ("_EAGLNativeWindowObject", "EAGLNativeWindowCallbacksRec",
                                                "__GLIFunctionDispatchRec", "__GLIContextRec", "_EAGLIOSurface")}
    if rec["structs"]["__GLIFunctionDispatchRec"]:
        rec["dispatch_fields"] = len(rec["structs"].pop("__GLIFunctionDispatchRec"))
    eagl_sels = implemented - NSOBJECT_SELS
    rec["consumers"] = {}
    for p, img in sorted(fw.images.items()):
        if p == OPENGLES or OPENGLES not in img.dylibs():
            continue
        imps = img.imports_from(OPENGLES, set(rec["exports"]))
        try:
            sels = sorted(img.selrefs() & eagl_sels)
        except (KeyError, struct.error):
            sels = ["(selrefs unreadable)"]
        rec["consumers"][p] = {"imports": sorted(imps), "weak": sorted(n for n, w in imps.items() if w), "eagl_selectors": sels}
    return rec


def markdown(recs):
    builds = [r["build"] for r in recs]
    out = []
    head = "| symbol | kind | " + " | ".join(builds) + " |\n|---|---|" + "---|" * len(builds)

    def row(name, k, have):
        return "| `%s` | %s | %s |" % (name, k, " | ".join("x" if h else "" for h in have))

    # 1. What consumers use, per consumer family
    used = {}
    for r in recs:
        for p, c in r.get("consumers", {}).items():
            leaf = p.rsplit("/", 1)[-1]
            for s in c["imports"]:
                used.setdefault(("import", s), {}).setdefault(r["build"], set()).add(leaf)
            for s in c["eagl_selectors"]:
                used.setdefault(("selector", s), {}).setdefault(r["build"], set()).add(leaf)
    out.append("## Symbols and selectors other images take from OpenGLES\n")
    out.append("Cells: which images use it on that build (blank: none).\n")
    out.append("| symbol / selector | kind | " + " | ".join(builds) + " |\n|---|---|" + "---|" * len(builds))
    for (what, s), per in sorted(used.items(), key=lambda x: (x[0][0], kind(x[0][1]) if x[0][0] == "import" else "", x[0][1])):
        k = kind(s) if what == "import" else ("EAGL selector (public)" if s in PUBLIC_SELS else "EAGL selector (private)")
        out.append("| `%s` | %s | %s |" % (s, k, " | ".join(", ".join(sorted(per.get(b, ()))) for b in builds)))
    # 2. Exports by kind and presence
    out.append("\n## OpenGLES exports by kind\n")
    out.append("| kind | " + " | ".join(builds) + " |\n|---|" + "---|" * len(builds))
    kinds = sorted({kind(s) for r in recs for s in r.get("exports", [])})
    for k in kinds:
        out.append("| %s | %s |" % (k, " | ".join(str(sum(1 for s in r.get("exports", []) if kind(s) == k)) for r in recs)))
    out.append("\n### Private and EAGL exports, per build\n")
    out.append(head)
    priv = sorted({s for r in recs for s in r.get("exports", []) if kind(s) not in ("gl core", "gl extension")})
    for s in priv:
        out.append(row(s, kind(s), [s in r.get("exports", []) for r in recs]))
    # 3. EAGL methods
    out.append("\n## EAGL classes' methods (OpenGLES's own ObjC metadata)\n")
    out.append("| class | method | " + " | ".join(builds) + " |\n|---|---|" + "---|" * len(builds))
    ms = sorted({(c, ("+" if side == "class" else "-") + m) for r in recs for c, v in r.get("classes", {}).items()
                 for side in ("instance", "class") for m in v[side]})
    for c, m in ms:
        have = [m[1:] in r.get("classes", {}).get(c, {}).get("class" if m[0] == "+" else "instance", []) for r in recs]
        out.append("| %s | `%s` | %s |" % (c, m, " | ".join("x" if h else "" for h in have)))
    out.append("\n## Selectors OpenGLES sends to other objects\n")
    out.append("| selector | " + " | ".join(builds) + " |\n|---|" + "---|" * len(builds))
    for s in sorted({s for r in recs for s in r.get("sends", [])}):
        out.append("| `%s` | %s |" % (s, " | ".join("x" if s in r.get("sends", []) else "" for r in recs)))
    out.append("\n## Private structures (@encode)\n")
    out.append("| structure | " + " | ".join(builds) + " |\n|---|" + "---|" * len(builds))
    for t in ("_EAGLNativeWindowObject", "EAGLNativeWindowCallbacksRec", "__GLIContextRec", "_EAGLIOSurface"):
        out.append("| `%s` | %s |" % (t, " | ".join(", ".join(r.get("structs", {}).get(t) or []) or "absent" for r in recs)))
    out.append("| `__GLIFunctionDispatchRec` fields | %s |" % " | ".join(str(r.get("dispatch_fields", "absent")) for r in recs))
    return "\n".join(out) + "\n"


PUBLIC_SELS = {"initWithAPI:", "initWithAPI:sharegroup:", "setCurrentContext:", "currentContext", "API", "sharegroup",
               "renderbufferStorage:fromDrawable:", "presentRenderbuffer:", "debugLabel", "setDebugLabel:",
               "isMultiThreaded", "setMultiThreaded:"}


def main(argv):
    if argv[:1] == ["--markdown"]:
        recs = json.load(open(argv[argv.index("--json") + 1]))
        open(argv[1], "w").write(markdown(recs))
        return 0
    out = None
    if argv[:1] == ["--json"]:
        out, argv = argv[1], argv[2:]
    recs = [survey(*a.split("=", 1)) for a in argv]
    s = json.dumps(recs, indent=1, sort_keys=True)
    if out:
        open(out, "w").write(s)
    else:
        print(s)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
