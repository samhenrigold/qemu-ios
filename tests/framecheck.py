#!/usr/bin/env python3
"""framecheck.py -- is a captured frame the right *picture*, not just lit?

The 2026-09-29 test audit (LightTouchMac docs/test-audit-2026-09-29.md, ranked gap #1)
showed every boot leg and matrix column judges liveness -- lit-pixel counts, "nothing
refused" -- so a frame written upside down, with red and blue swapped, or left stale
(a previous surface) all PASS. Section 4 of the audit also showed the fix: the GL path
renders the software-CoreAnimation path's pixels to within 1 LSB on the screens we can
pin (2.x/1.x home and Safari, the iPad SpringBoard), captured over the same QMP
screendump. So a reference *does* work here -- as long as it is a downsampled, tolerant
signature (block means, not pixel-exact: robust to host-GPU filtering) rather than a
golden PNG.

The reference is a `GW`-wide box-filter downsample of a known-good frame, stored as a
tiny PNG (a few KB, human-inspectable, so the repo stays small). `verdict` downsamples
the capture the same way and reports the fraction of blocks (status-bar/clock band
masked) that differ by more than TOL. Measured margins on the audit's captures: a
correct frame is 0.000-0.007; flip 0.30-0.69, R/B swap 0.21-0.31, a stale iPad surface
0.10 -- so THR 0.02 separates them with room on both sides.

    framecheck.py make REF.png IMG            build a reference from a known-good frame
    framecheck.py check REF.png IMG [--thr F]    diff a capture against it
    framecheck.py selftest                     tiny built-in assert demo
"""
import sys

GW = 64          # signature grid width; height is aspect-scaled from the source
TOL = 8          # per-channel block-mean drift still counted as "the same block"
THR = 0.02       # FAIL if more than this fraction of unmasked blocks differ by > TOL
MASK_TOP = 0.08  # top band excluded: the status-bar clock (and the iPad lock time)


def signature(path, gw=GW):
    """A frame -> (gw, gh, [ (r,g,b), ... ]) of block means. Idempotent on a reference
    PNG that is already gw wide. Needs Pillow."""
    from PIL import Image
    im = Image.open(path).convert("RGB")
    w, h = im.size
    gh = max(1, round(gw * h / w))
    small = im.resize((gw, gh), Image.BOX)  # BOX = block-mean downsample
    return gw, gh, list(small.getdata())


def fraction_differing(cap, ref, tol=TOL, mask_top=MASK_TOP):
    """Fraction of unmasked blocks whose max channel drift exceeds `tol`."""
    (gw, gh, a), (rw, rh, b) = cap, ref
    if (gw, gh) != (rw, rh):
        raise ValueError("aspect/grid mismatch: capture %dx%d vs reference %dx%d" % (gw, gh, rw, rh))
    top = int(gh * mask_top)
    differ = total = 0
    for i in range(top * gw, gh * gw):
        pa, pb = a[i], b[i]
        if max(abs(pa[0] - pb[0]), abs(pa[1] - pb[1]), abs(pa[2] - pb[2])) > tol:
            differ += 1
        total += 1
    return differ / total if total else 0.0


def verdict(cap_path, ref_path, thr=THR):
    """{ok, frac, thr, why} for a captured frame against a reference PNG."""
    try:
        ref = signature(ref_path)
        cap = signature(cap_path, ref[0])
        frac = fraction_differing(cap, ref)
    except Exception as e:  # aspect mismatch, unreadable frame: not the reference picture
        return {"ok": False, "frac": None, "thr": thr, "why": "could not compare: %s" % e}
    ok = frac <= thr
    return {"ok": ok, "frac": round(frac, 4), "thr": thr,
            "why": ("matches the reference (%.3f <= %.2f)" % (frac, thr)) if ok else
                   ("differs from the reference (%.3f > %.2f): flip / color swap / stale surface"
                    % (frac, thr))}


def make_ref(src_path, out_png, gw=GW):
    """Write a known-good frame's downsample as the reference PNG."""
    from PIL import Image
    gw, gh, grid = signature(src_path, gw)
    im = Image.new("RGB", (gw, gh))
    im.putdata(grid)
    im.save(out_png)


def brightness(path):
    """Mean luma 0..1. A slept/black panel is ~0; the audit's black home is exactly this."""
    from PIL import Image
    px = list(Image.open(path).convert("L").getdata())
    return sum(px) / (len(px) * 255) if px else 0.0


def _selftest():
    black = (2, 2, [(0, 0, 0)] * 4)
    assert fraction_differing(black, black) == 0.0
    red = (2, 2, [(255, 0, 0)] * 4)
    assert fraction_differing(black, red) == 1.0        # every unmasked block differs
    assert fraction_differing(black, red, tol=255) == 0.0  # tolerance swallows it
    print("framecheck selftest ok")


def main(argv):
    if len(argv) >= 2 and argv[1] == "selftest":
        return _selftest()
    if len(argv) >= 4 and argv[1] == "make":
        make_ref(argv[3], argv[2])
        print("wrote %s from %s" % (argv[2], argv[3]))
        return
    if len(argv) >= 4 and argv[1] == "check":
        thr = float(argv[argv.index("--thr") + 1]) if "--thr" in argv else THR
        import json
        v = verdict(argv[3], argv[2], thr)
        print(json.dumps(v))
        sys.exit(0 if v["ok"] else 1)
    sys.exit(__doc__)


if __name__ == "__main__":
    main(sys.argv)
