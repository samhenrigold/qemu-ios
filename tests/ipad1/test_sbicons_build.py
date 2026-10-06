#!/usr/bin/env python3
"""app-compat's icon lookup builds sbicons on a worktree that lacks it, instead of reading as "no slot"."""
import importlib.util, plistlib, tempfile
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('ipad1_app_compat', Path(__file__).with_name('app-compat.py'))
ac = importlib.util.module_from_spec(spec); spec.loader.exec_module(ac)

state = [[], [[False, {'bundleIdentifier': 'com.qemuios.harness'}]]]   # dock, page 1
with tempfile.TemporaryDirectory(prefix='sbicons-') as tmp:
    binary = Path(tmp) / 'build' / 'ipad1-tools' / 'sbicons'
    compiled = []

    def run(argv, **kw):
        if argv[0] == 'pkg-config':
            return SimpleNamespace(stdout='-limobiledevice-1.0\n', returncode=0)
        assert argv[0] == 'cc' and ac.SBICONS_SRC in argv, argv
        compiled.append(argv)
        Path(argv[argv.index('-o') + 1]).write_bytes(b'compiled')
        return SimpleNamespace(returncode=0, stderr='')

    b = SimpleNamespace(run=lambda argv, timeout: SimpleNamespace(
        returncode=0 if argv == [str(binary)] else 1, stdout=plistlib.dumps(state).decode()))
    with patch.object(ac, 'SBICONS', str(binary)), patch.object(ac.subprocess, 'run', side_effect=run):
        assert ac.icon_slot(b, 'com.qemuios.harness') == (1, 0, 1)
        assert ac.icon_slot(b, 'com.qemuios.harness') == (1, 0, 1)
    assert len(compiled) == 1 and binary.read_bytes() == b'compiled'
print('PASS icon_slot builds a missing sbicons once and reads the slot')
