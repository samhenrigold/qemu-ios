"""Declared deployment compatibility only; guest API/ABI acceptance is separate."""
import hashlib
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


def app_metadata(path):
    """One shared IPA/app plist reader for eligibility and input receipts."""
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
    return info


def minimum_problem(path, guest_version):
    """Read an IPA or app bundle; passing proves only its declared minimum."""
    guest = version(guest_version)
    declared = app_metadata(path).get('MinimumOSVersion')
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
        wanted.append(('Harness', cfg.harness_ipa or harness_default, getattr(cfg, 'harness_explicit', bool(cfg.harness_ipa))))
    if 'gles' in selected and not cfg.gles_front_end:
        app = cfg.gles_app or gles_default
        if Path(app).exists() or getattr(cfg, 'gles_explicit', bool(cfg.gles_app)):
            wanted.append(('GLES app', app, getattr(cfg, 'gles_explicit', bool(cfg.gles_app))))
        else:
            wanted.append(('GLES Harness fallback', cfg.harness_ipa or harness_default, getattr(cfg, 'harness_explicit', bool(cfg.harness_ipa))))
    problems = []
    if "gles" in selected and getattr(cfg, "slotmap_explicit", bool(getattr(cfg, "gles_slotmap", None))):
        if not Path(cfg.gles_slotmap).is_file():
            problems.append("GLES: missing explicit slot ABI map: " + cfg.gles_slotmap)
    seen = set()
    for label, path, explicit in wanted:
        if (path, explicit) in seen:
            continue
        seen.add((path, explicit))
        if not Path(path).exists():
            if explicit or getattr(cfg, "fixture_flavor", None) == "ios2":
                problems.append('%s: missing %s fixture: %s; build the selected flavor' % (label, 'explicit' if explicit else 'ios2', path))
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


def select_inputs(root, guest_version, *, ipa=None, harness=None, gles=None, slotmap=None):
    """Version-family fixture eligibility; only N72/2.1.1 has native evidence.

    Pure selection never inspects presence and never crosses flavors on absence.
    """
    root = Path(root)
    legacy = guest_version is not None and version(guest_version)[0] == 2
    flavor = 'ios2' if legacy else 'full'
    default_harness = root / 'contrib/it-harness' / ('build-ios2' if legacy else 'build') / 'Harness.ipa'
    default_gles = root / 'contrib/it-gles' / ('build-ios2/GLTest.app' if legacy else 'GLTest.app')
    return {'flavor': flavor,
            'ipa': str(ipa if ipa is not None else default_harness),
            'harness': str(harness if harness is not None else default_harness),
            'gles': str(gles if gles is not None else default_gles),
            'slotmap': str(slotmap if slotmap is not None else root / 'contrib/it-gles/slotmap.txt'),
            'explicit': {'ipa': ipa is not None, 'harness': harness is not None,
                         'gles': gles is not None, 'slotmap': slotmap is not None}}


def _sha256(path):
    checksum = hashlib.sha256()
    with open(path, 'rb') as file:
        for block in iter(lambda: file.read(65536), b''):
            checksum.update(block)
    return checksum.hexdigest()


def selected_metadata(cfg, selected):
    """Content identity for requested inputs only; no inferred runtime success."""
    roles = []
    if {'appinstall', 'applaunch'} & set(selected):
        roles.append(('ipa', cfg.ipa, cfg.ipa_explicit))
    if 'audio' in selected:
        roles.append(('harness', cfg.harness_ipa, cfg.harness_explicit))
    if 'gles' in selected and not cfg.gles_front_end:
        if Path(cfg.gles_app).exists() or cfg.gles_explicit:
            roles.append(('gles', cfg.gles_app, cfg.gles_explicit))
        else:
            roles.append(('gles-harness-fallback', cfg.harness_ipa, cfg.harness_explicit))
        roles.append(('slotmap', cfg.gles_slotmap, cfg.slotmap_explicit))

    rows = []
    for role, path, explicit in roles:
        path = Path(path)
        row = {
            'role': role,
            'path': str(path),
            'source': 'explicit' if explicit else cfg.fixture_flavor,
            'guestVersion': cfg.product_version,
            'status': 'missing',
        }
        if path.exists():
            row['status'] = 'present'
            if role == 'slotmap':
                row['sha256'] = _sha256(path)
            else:
                info = app_metadata(path)
                if path.is_dir():
                    executable = info.get('CFBundleExecutable')
                    if (not isinstance(executable, str) or not executable
                            or Path(executable).name != executable):
                        raise ValueError('invalid declared app executable for input receipt')
                    row['plistSHA256'] = _sha256(path / 'Info.plist')
                    row['executableSHA256'] = _sha256(path / executable)
                else:
                    row['sha256'] = _sha256(path)
                row['minimumOS'] = info.get('MinimumOSVersion')
        rows.append(row)

    return {
        'schemaVersion': 1,
        'declaredGuestVersion': cfg.product_version,
        'limits': ('Content identity and declared minimum only; this receipt establishes '
                   'no native runtime success. Maintained legacy Harness evidence is '
                   'limited to N72/2.1.1, and does not qualify GL or other versions/features'),
        'inputs': rows,
    }
