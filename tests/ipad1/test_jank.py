#!/usr/bin/env python3
"""tests/ipad1/jank.py's scoring on synthetic frame timelines (host only): a clean 60 Hz animation, one held
swap (the IT_JANK_STALL_EVERY shape), a stall long enough to split the animation, and the measured window."""
import importlib.util, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("jank", os.path.join(HERE, "jank.py"))
J = importlib.util.module_from_spec(spec); spec.loader.exec_module(J)
V = 1_000_000_000 // 60


def timeline(newflags):
    """One entry per vsync: (seq, virt_ns, newframe, key)."""
    return [(i, i * V, f, i if f else 0) for i, f in enumerate(newflags)]


clean = J.analyze(timeline([1] * 30), -1, 1000)
assert (clean["frames"], clean["runs"], clean["hitches_gt_33ms"], clean["dropped"]) == (30, 1, 0, 0), clean
assert abs(clean["p99_ms"] - 16.7) < 0.1, clean

held = J.analyze(timeline([1] * 10 + [0, 0] + [1] * 10), -1, 1000)           # one frame shown for 3 vsyncs
assert (held["runs"], held["hitches_gt_33ms"], held["dropped"]) == (1, 1, 2), held
assert abs(held["longest_ms"] - 50.0) < 0.1, held

late = J.analyze(timeline([1] * 10 + [0] + [1] * 10), -1, 1000)            # one frame a vsync late: 33.3 ms
assert (late["hitches_gt_33ms"], late["dropped"]) == (1, 1), late

split = J.analyze(timeline([1] * 10 + [0] * 12 + [1] * 10), -1, 1000)        # a 217 ms stall mid-animation
assert (split["runs"], split["hitches_gt_33ms"]) == (2, 0), split           # caught as an extra run

windowed = J.analyze(timeline([1] * 30 + [0] * 30 + [1] * 30), -1, 400)     # only the first 400 ms count
assert windowed["frames"] == 25, windowed

base = {"x": {"min_frames": 20, "max_p99_ms": 20, "max_hitches": 0, "max_longest_ms": 20, "max_dropped": 0,
              "max_runs": 1}}
J.BASELINES = os.devnull
J.json.load = lambda f: base
assert J.gate({"x": clean}) and not J.gate({"x": held}) and not J.gate({"x": split})
print("test_jank: ok")
