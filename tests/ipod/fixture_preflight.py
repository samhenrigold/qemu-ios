"""Declared deployment compatibility only; guest API/ABI acceptance is separate."""
from pathlib import Path
import plistlib
import re
import zipfile
from xml.parsers.expat import ExpatError


def version(value):
    if not isinstance(value, str) or not re.fullmatch(r'[0-9]+(?:\.[0-9]+){0,3}', value):
        raise ValueError('invalid declared iOS version: %r' % value)
    parts = tuple(int(x) for x in value.split('.'))
    return parts + (0,) * (4 - len(parts))


def minimum_problem(path, guest_version):
    """Read an IPA or app bundle; passing proves only its declared minimum."""
    guest = version(guest_version)
    path = Path(path)
    if path.is_dir():
        info = plistlib.loads((path / 'Info.plist').read_bytes())
    else:
        with zipfile.ZipFile(path) as archive:
            names = [name for name in archive.namelist()
                     if re.fullmatch(r'Payload/[^/]+\.app/Info\.plist', name)]
            if len(names) != 1:
                raise ValueError('IPA must contain exactly one top-level app Info.plist')
            info = plistlib.loads(archive.read(names[0]))
    if not isinstance(info, dict):
        raise ValueError('app Info.plist must be a dictionary')
    declared = info.get('MinimumOSVersion')
    if declared is None:
        return 'fixture has no declared MinimumOSVersion'
    if version(declared) > guest:
        return 'fixture requires iOS %s; guest is iOS %s; rebuild a compatible fixture' % (declared, guest_version)
    return None


def requested_problems(cfg, selected, harness_default, gles_default):
    """Reject selected incompatible inputs before guest/services/output mutation.

    Missing default inputs retain the harness's existing per-check skip policy.
    Explicit inputs require a known guest version and readable metadata.
    """
    wanted = []
    if {'appinstall', 'applaunch'} & set(selected):
        wanted.append(('IPA', cfg.ipa, cfg.ipa_explicit))
    if 'audio' in selected:
        wanted.append(('Harness', cfg.harness_ipa or harness_default, bool(cfg.harness_ipa)))
    if 'gles' in selected and not cfg.gles_front_end:
        app = cfg.gles_app or gles_default
        if Path(app).exists() or cfg.gles_app:
            wanted.append(('GLES app', app, bool(cfg.gles_app)))
        else:
            wanted.append(('GLES Harness fallback', cfg.harness_ipa or harness_default, bool(cfg.harness_ipa)))
    problems = []
    if "gles" in selected and getattr(cfg, "gles_slotmap", None):
        if not Path(cfg.gles_slotmap).is_file():
            problems.append("GLES: missing explicit slot ABI map: " + cfg.gles_slotmap)
    seen = set()
    for label, path, explicit in wanted:
        if (path, explicit) in seen:
            continue
        seen.add((path, explicit))
        if not Path(path).exists():
            if explicit:
                problems.append('%s: missing explicit fixture: %s' % (label, path))
            continue
        if cfg.product_version is None and not explicit:
            continue  # Historical raw NAND inputs have no declared guest version.
        try:
            why = minimum_problem(path, cfg.product_version)
        except (OSError, ValueError, KeyError, zipfile.BadZipFile, plistlib.InvalidFileException, ExpatError) as error:
            why = 'cannot validate declared compatibility: %s' % error
        if why:
            problems.append('%s (%s): unsupported input: %s' % (label, path, why))
    return problems
