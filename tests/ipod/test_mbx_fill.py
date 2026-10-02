#!/usr/bin/env python3
"""S5LBox-derived BGRA fill leaf: captured N72 encoding, synthetic RAM only."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="mbx-fill-") as td:
    binary = str(Path(td) / "test")
    subprocess.run([
        "clang", "-std=c11", "-g", "-O1", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
        "-I", str(root / "include"), str(root / "tests/ipod/fixtures/mbx_fill.c"),
        str(root / "hw/arm/mbx_fill.c"), "-o", binary,
    ], check=True)
    subprocess.run([binary], check=True)
