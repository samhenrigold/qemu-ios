#!/usr/bin/env python3
"""The gate inventory is explicit and fails closed for new/missing tests."""
import copy
import importlib.util
import json
from pathlib import Path
import tempfile
import shutil
import subprocess
import os

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('gate_registry', ROOT / 'tests/gate_registry.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
registry = json.loads((ROOT / 'tests/gate-registry.json').read_text())
entries = module.validate(ROOT, registry)
assert {entry['tier'] for entry in entries} == module.TIERS
assert all(entry['missing'] == 'fail' for entry in entries if entry['tier'] == 'models')
assert next(e for e in entries if e['name'].endswith('test_h264_native.py'))['tier'] == 'manual'
assert next(e for e in entries if e['name'].endswith('test_pcm_capture.py'))['prerequisites'] == ['capture', 'reference']


def rejected(root, data, message):
    try:
        module.validate(root, data)
    except ValueError as error:
        assert message in str(error), error
    else:
        raise AssertionError(f'accepted invalid registry: {message}')


with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    path = root / 'tests/ipod/test_example.py'
    path.parent.mkdir(parents=True)
    # Tokens that formerly changed scheduling are irrelevant to registration.
    path.write_text('# qemu-system-arm\n# sys.exit(__doc__)\n')
    data = {'version': 1, 'tests': [{'name': 'tests/ipod/test_example.py', 'tier': 'quick', 'prerequisites': []}]}
    assert module.validate(root, data)[0]['tier'] == 'quick'
    extra = path.with_name('test_new.py')
    extra.touch()
    rejected(root, data, 'unregistered tests: tests/ipod/test_new.py')
    extra.unlink()
    duplicate = copy.deepcopy(data)
    duplicate['tests'].append(duplicate['tests'][0])
    rejected(root, duplicate, 'duplicate registration')
    manual = copy.deepcopy(data)
    manual['tests'][0]['tier'] = 'manual'
    rejected(root, manual, 'manual check must declare')
    # Exercise the actual shell gate: declared host usage failures cannot skip,
    # and unregistered checks stop execution before the first test runs.
    (root / 'tests/gate-registry.json').write_text(json.dumps(data))
    for helper in ('gate.sh', 'gate_registry.py'):
        shutil.copyfile(ROOT / 'tests' / helper, root / 'tests' / helper)
    path.write_text('import sys\nprint("usage: required-input")\nsys.exit(2)\n')
    out = root / 'output'
    env = dict(os.environ, OUT=str(out), JOBS='1')
    gate = subprocess.run(['bash', str(root / 'tests/gate.sh'), '--quick'], env=env, capture_output=True, text=True)
    assert gate.returncode == 1 and 'FAIL' in (out / 'results').read_text(), gate.stdout + gate.stderr
    extra.touch()
    path.write_text('raise AssertionError("must not execute unregistered gate")\n')
    gate = subprocess.run(['bash', str(root / 'tests/gate.sh'), '--models'], env=env, capture_output=True, text=True)
    assert gate.returncode == 1 and 'unregistered tests' in gate.stderr, gate.stdout + gate.stderr
    assert not (out / 'results').read_text()
    extra.unlink()
    path.unlink()
    rejected(root, data, 'registered test missing')
print('PASS explicit gate registration, prerequisites, source independence, missing and unregistered tests')
