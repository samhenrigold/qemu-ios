#!/usr/bin/env python3
"""Exercise production reference selection and frame verdicts without booting QEMU."""
import ast
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import tempfile
from types import SimpleNamespace
from PIL import Image

root = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("framecheck", root / "tests/framecheck.py")
framecheck = importlib.util.module_from_spec(spec)
spec.loader.exec_module(framecheck)
source = ast.parse((root / "tests/ipad1/regress.py").read_text())
functions = [node for node in source.body if isinstance(node, ast.FunctionDef)
             and node.name in ("qualified_frame_reference", "gl_clean")]
assert len(functions) == 2
scope = dict(os=os, re=re, json=json, hashlib=hashlib, framecheck=framecheck, MAGENTA_MAX=0.001,
             itqmp=SimpleNamespace(gles_rejects=lambda _: {}, magenta_fraction=lambda *a, **kw: 0))
exec(compile(ast.Module(body=functions, type_ignores=[]), "production-frame-checks", "exec"), scope)

class Result:
    def set(self, ok, detail):
        self.ok, self.detail = ok, detail

with tempfile.TemporaryDirectory(prefix="ipad-frame-reference-") as tmp:
    directory = Path(tmp)
    scope["GLES_REFS"] = tmp
    cfg = SimpleNamespace(build="7B500", product_version="3.2.2", gl_test=False)
    serial = directory / "serial.log"
    serial.write_text("CoreAnimation composites through the host\n")
    boot = SimpleNamespace(cfg=cfg, serial=str(serial), qmp=None)
    # Synthetic test pictures are fixture inputs, never committed goldens.
    image = Image.new("RGB", (128, 96))
    image.putdata([(x * 2, y * 2, (x + y) % 256) for y in range(96) for x in range(128)])
    capture = directory / "home.png"
    image.save(capture)
    framecheck.make_ref(capture, directory / "ipad-home.png")
    result = Result()
    scope["gl_clean"](boot, result, "fixture", [str(capture)], require_refs=("home",))
    assert not result.ok and "missing qualified" in result.detail, result.detail
    reference = directory / "k48ap/7B500"
    reference.mkdir(parents=True)
    framecheck.make_ref(capture, reference / "home.png")
    manifest = dict(build="7B500", product_version="3.2.2",
                    baseline=dict(renderer="stock-software-coreanimation", provenance="synthetic unit fixture only"),
                    frames=dict(home=dict(sha256=hashlib.sha256((reference / "home.png").read_bytes()).hexdigest(),
                                          source_sha256=hashlib.sha256(capture.read_bytes()).hexdigest())))
    manifest_path = reference / "reference.json"
    manifest_path.write_text(json.dumps(manifest))
    scope["gl_clean"](boot, result, "fixture", [str(capture)], require_refs=("home",))
    assert result.ok and "qualified frame-ref home" in result.detail
    # Same-qualified baseline rejects incorrect orientation; tolerance unchanged.
    image.transpose(Image.Transpose.FLIP_TOP_BOTTOM).save(capture)
    scope["gl_clean"](boot, result, "fixture", [str(capture)], require_refs=("home",))
    assert not result.ok and "differs from the reference" in result.detail
    cfg.build = None
    assert "missing firmware build identity" in scope["qualified_frame_reference"](cfg, "home")[1]
    cfg.build = "8C148"
    path, why = scope["qualified_frame_reference"](cfg, "home")
    assert path is None and "missing qualified" in why
    cfg.build = "7B500"
    manifest["product_version"] = "4.2.1"
    manifest_path.write_text(json.dumps(manifest))
    assert "identity differs" in scope["qualified_frame_reference"](cfg, "home")[1]
    manifest["product_version"] = "3.2.2"
    manifest["baseline"]["renderer"] = "candidate-gl"
    manifest_path.write_text(json.dumps(manifest))
    assert "independent" in scope["qualified_frame_reference"](cfg, "home")[1]
    manifest["baseline"]["renderer"] = "stock-software-coreanimation"
    manifest_path.write_text(json.dumps(manifest))
    (reference / "home.png").write_bytes(b"tampered")
    assert "hash differs" in scope["qualified_frame_reference"](cfg, "home")[1]
    scope["gl_clean"](boot, result, "liveness-only fixture", [])
    assert result.ok and "no frame-reference comparison" in result.detail
print("PASS: build-scoped references, explicit missing coverage, provenance/hash/OS checks, unchanged flipped-frame rejection, accurate liveness evidence")
