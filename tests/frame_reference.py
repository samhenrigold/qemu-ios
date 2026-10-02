"""Build-scoped visual references shared by native device regression gates.

A manifest describes reviewed independent baseline evidence; its presence alone
is not compatibility proof. Never borrow another firmware's icon layout.
"""
import hashlib
import json
from pathlib import Path
import re


def qualified(root, board, build, product_version, scene):
    if not isinstance(board, str) or not re.fullmatch(r"[a-z0-9]+", board):
        return None, "missing board identity"
    if not isinstance(build, str) or not re.fullmatch(r"[A-Za-z0-9]+", build):
        return None, "missing firmware build identity"
    if not isinstance(scene, str) or not re.fullmatch(r"[a-z][a-z0-9-]*", scene):
        return None, "invalid scene identity"
    if not isinstance(product_version, str) or not re.fullmatch(r"[0-9]+(?:\.[0-9]+)*", product_version):
        return None, "missing full firmware product version"
    directory = Path(root) / board / build
    label = f"{board}/{build}/{scene}"
    manifest_path = directory / "reference.json"
    if not manifest_path.is_file():
        return None, f"missing qualified frame reference for {label} (independent baseline required)"
    try:
        manifest = json.loads(manifest_path.read_text())
        if (manifest.get("build") != build or manifest.get("product_version") != product_version
                or manifest.get("board", board) != board):
            raise ValueError("reference firmware identity differs from the device")
        baseline = manifest.get("baseline", {})
        if (baseline.get("renderer") not in ("stock-software-coreanimation", "stock-software-layerkit", "physical-device")
                or not baseline.get("provenance")):
            raise ValueError("reference needs independent software/physical baseline provenance")
        frame = manifest.get("frames", {}).get(scene)
        if not frame:
            return None, f"missing qualified frame reference for {label}"
        for key in ("sha256", "source_sha256"):
            if not re.fullmatch(r"[0-9a-f]{64}", frame.get(key, "")):
                raise ValueError(f"reference lacks {key}")
        reference = directory / (scene + ".png")
        if hashlib.sha256(reference.read_bytes()).hexdigest() != frame["sha256"]:
            raise ValueError("reference PNG hash differs from its manifest")
        return str(reference), None
    except (OSError, ValueError, TypeError, AttributeError) as error:
        return None, f"invalid qualified frame reference: {error}"
