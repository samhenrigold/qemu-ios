#!/usr/bin/env python3
"""test_framecheck.py -- the frame reference catches the audit's three graphics mutants.

The 2026-09-29 audit (LightTouchMac docs/test-audit-2026-09-29.md, gap #1) showed every
boot leg and matrix column judged liveness, so an upside-down frame (flip), a red/blue
swap and a stale surface all PASSed. framecheck.py diffs a capture against a committed
software-CA reference (clock band masked). This proves, without a live boot, that:
  - a correct frame (a *different* good capture than the reference was built from) passes;
  - each mutant fails on a screen where it manifests.
Fixtures under framecheck-fixtures/ are downsampled captures from the audit's own
flip/rbswap/stale builds (patches in ~/Developer/audit-0929/{flip,rbswap,stale}.patch)."""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
import framecheck

REFS = os.path.join(HERE, "..", "gles-refs")
FX = os.path.join(HERE, "framecheck-fixtures")

# (fixture, reference, expect_ok, label)
CASES = [
    ("correct-2x-home.png",   "2x-home.png",     True,  "2.x correct home passes"),
    ("correct-1x-home.png",   "1x-home.png",     True,  "1.x correct home passes"),
    ("flip-2x-home.png",      "2x-home.png",     False, "flip mutant (2.x home) fails"),
    ("flip-1x-home.png",      "1x-home.png",     False, "flip mutant (1.x home) fails"),
    ("stale-1x-home.png",     "1x-home.png",     False, "stale mutant (1.x home) fails"),
    ("stale-ipad-screen.png", "ipad-screen.png", False, "stale mutant (iPad screen) fails"),
    ("rbswap-ipad-home.png",  "ipad-home.png",   False, "R/B swap mutant (iPad home) fails"),
]

fails = 0
for fx, ref, expect_ok, label in CASES:
    v = framecheck.verdict(os.path.join(FX, fx), os.path.join(REFS, ref))
    ok = (v["ok"] == expect_ok)
    print("%-4s %-38s frac=%s (%s)" % ("PASS" if ok else "FAIL", label, v["frac"], v["why"]))
    if not ok:
        fails += 1

# The mask must not swallow a real mismatch, and tolerance must swallow host-GPU jitter.
framecheck._selftest()

if fails:
    sys.exit("%d framecheck case(s) misclassified" % fails)
print("framecheck: correct frames pass, all three mutants fail")
