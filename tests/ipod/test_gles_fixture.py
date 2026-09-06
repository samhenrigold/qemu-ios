#!/usr/bin/env python3
"""Missing GLTest must select Harness without silently replacing guest graphics."""
import os
import tempfile
from types import SimpleNamespace
from unittest.mock import patch, Mock
import regress as R

with tempfile.TemporaryDirectory() as directory:
    cfg = SimpleNamespace(out=directory, install_timeout=30, stage_gles_shim=False)
    dev = SimpleNamespace(dir=directory, qmp=Mock(), serial_text=lambda: '')
    dev.qmp.shot.return_value = 'frame.ppm'
    response = SimpleNamespace(returncode=0, stdout='', stderr='')
    for signature, expected in (((.3, .3), True), ((0, 0), False)):
        result = R.Result('gles')
        with patch.object(R.os.path, 'exists', side_effect=lambda p: not str(p).endswith('GLTest.app')), \
             patch.object(R, 'prepare_app_control', return_value=123), \
             patch.object(R, 'app_is_installed', return_value=True), \
             patch.object(R, 'unlock', return_value=(True, '')), \
             patch.object(R, 'springboard', return_value=response) as launch, \
             patch.object(R, 'foreground_is', return_value=True), \
             patch.object(R, 'guest_ssh', return_value=response) as ssh, \
             patch.object(R, 'quad_signature', return_value=signature), \
             patch.object(R, 'lit_count', return_value=(255, 200000)), \
             patch.object(R, 'to_png'), patch.object(R.time, 'sleep'), patch.object(R, 'log'):
            assert R.check_gles(cfg, None, dev, result) is expected, result.detail
            launch.assert_called_once_with(cfg, 123, 'com.qemuios.harness')
            dev.qmp.tap.assert_called_with(150, 79)
            assert all('scp_from' not in call.kwargs for call in ssh.call_args_list)
print('PASS: Harness fallback drives GL row, rejects absent colors, preserves baked renderer')
