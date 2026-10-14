#!/usr/bin/env python3
"""Summarizes -icount shift=0,sleep=off probe runs: guest MIPS over the 1e9..5e9 instruction stretch of
the boot (CPU-bound; sleep=off warps idle time away) and when the lock screen first lit.
usage: mips.py RUN-NAME..."""
import os, sys

for name in sys.argv[1:]:
    rows = []
    for line in open(os.path.expanduser(f"~/Developer/wasm-spike-run/{name}/stderr.log")):
        if line.startswith("spike "):
            f = dict(kv.split("=") for kv in line.split()[1:])
            rows.append((float(f["t"]), int(f["icount"]), float(f["luma"])))
    lit = next(((t, c) for t, c, l in rows if l > 0.1), None)
    t1 = next((t for t, c, _ in rows if c >= 1e9), None)
    t5 = next((t for t, c, _ in rows if c >= 5e9), None)
    mips = f"{4e3 / (t5 - t1):.0f} MIPS (1e9..5e9 insns in {t5 - t1:.1f} s)" if t1 and t5 else "n/a"
    print(f"{name}: {mips}; lit at " + (f"{lit[0]:.0f} s, {lit[1] / 1e9:.2f}e9 insns" if lit else "never")
          + f"; last t={rows[-1][0]:.0f} s icount={rows[-1][1] / 1e9:.2f}e9")
