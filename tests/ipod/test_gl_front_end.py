#!/usr/bin/env python3
"""ipod2g_device.gl_front_end puts the one GL front end (contrib/gles-public/OpenGLES) over OpenGLES.framework/OpenGLES
on every iPod build: 2.x/3.0 (no shared cache) replace the stock file, 3.1+ (OpenGLES cached) also get dyld's
enable-dylibs-to-override-cache switch, a dyld without it fails. And every package family that hooks GL hooks that
same binary (1.x's own front end aside)."""
import os, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path[:0] = [os.path.join(ROOT, "imgtools"), os.path.join(ROOT, "contrib/guest-package")]
import ipod2g_device as d
import ipad1_rootfs as r
import mkpkg


def volume(t, cached, switch=True):
    os.makedirs(os.path.join(t, os.path.dirname(d.OPENGLES)))
    open(os.path.join(t, d.OPENGLES), "wb").write(b"stock")
    os.makedirs(os.path.join(t, "usr/lib"))
    open(os.path.join(t, "usr/lib/dyld"), "wb").write(b"x\0/%s\0" % r.DYLD_OVERRIDE.encode() if switch else b"no switch")
    if cached:   # a dyld_v1 cache naming OpenGLES
        path = b"/" + d.OPENGLES.encode() + b"\0"
        img = bytearray(b"dyld_v1   armv6\0" + __import__("struct").pack("<4I", 0x40, 1, 0x60, 1)).ljust(0x40, b"\0")
        img += __import__("struct").pack("<QQQ", 0, 0x1000, 0).ljust(32, b"\0")
        img += __import__("struct").pack("<QQQI", 0, 0, 0, 0x100).ljust(32, b"\0")
        os.makedirs(os.path.join(t, os.path.dirname(d.DYLD_CACHE)))
        open(os.path.join(t, d.DYLD_CACHE), "wb").write(bytes(img.ljust(0x100, b"\0")) + path)
    return t


built = os.path.join(ROOT, d.GL_FRONT_END)
if os.path.exists(built):
    front = open(built, "rb").read()
    for cached in (False, True):
        with tempfile.TemporaryDirectory() as t:
            owners = []
            line = d.gl_front_end(volume(t, cached), owners)
            assert open(os.path.join(t, d.OPENGLES), "rb").read() == front, line
            assert ("overridden" in line) == cached and os.path.exists(os.path.join(t, r.DYLD_OVERRIDE)) == cached, line
            assert ("0 0", d.OPENGLES) in owners and (("0 0", r.DYLD_OVERRIDE) in owners) == cached, owners
    with tempfile.TemporaryDirectory() as t:
        try:
            d.gl_front_end(volume(t, True, switch=False), [])
            assert False, "a cached OpenGLES and a dyld without the override switch must fail the bake"
        except SystemExit:
            pass
    # legacy-linked (2.x/3.0 dyld) and signed, both slices
    for arch in ("armv6", "armv7"):
        assert mkpkg.macho_problem(front, arch, legacy=True, signed=True) is None, arch
# every GL hook of every iPod 2G / iPad family is the one front end
hooks = {f: dict((t, s) for s, t, _ in spec.get("hooks", [])) for f, spec in mkpkg.FAMILIES.items()}
for fam in ("n72-ios2", "n72-ios30", "n72-ios3", "k48-ios3", "k48-ios4", "k48-ios5"):
    assert hooks[fam][mkpkg.OPENGLES] == d.GL_FRONT_END, fam
print("PASS: the GL front end over OpenGLES (no cache: replaced; cached: + override switch; no switch: refused)%s, "
      "every GL hook the same binary" % (", legacy and signed" if os.path.exists(built) else " (not built: install checks skipped)"))
