#!/usr/bin/env python3
"""A successful kill or an unrelated reply must not pass a respring check."""
import tempfile
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import Mock, patch
import regress as R


def reply(text="", rc=0):
    return SimpleNamespace(stdout=text, stderr="", returncode=rc)


with tempfile.TemporaryDirectory() as directory, \
     patch.object(R, "prepare_app_control", return_value=22), \
     patch.object(R.time, "sleep"), patch.object(R, "log"):
    dev = SimpleNamespace(dir=directory, qmp=Mock(reset_count=0))
    result = R.Result("respring")
    with patch.object(R, "guest_ssh", side_effect=[reply(), reply(rc=124),
                      reply("sblaunch: locked=1 passcode=0")]):
        assert R.check_respring(None, None, dev, result)

    for response in (reply(rc=124), reply("unrelated successful reply")):
        result = R.Result("respring")
        with patch.object(R.time, "monotonic", side_effect=[0, 0, 1, 46]), \
             patch.object(R, "guest_ssh", side_effect=[reply(), response,
                          reply("guest crash report")]):
            assert not R.check_respring(None, None, dev, result)
            assert "did not recover" in result.detail
            assert "guest crash report" in Path(directory, "respring-diagnostics.txt").read_text()

    def status(*args):
        if dev.qmp.cmd.call_count > initial_calls + 1:
            dev.qmp.reset_count += 1
    initial_calls = dev.qmp.cmd.call_count
    dev.qmp.cmd.side_effect = status
    result = R.Result("respring")
    with patch.object(R, "guest_ssh", side_effect=[reply(),
                      reply("sblaunch: locked=0 passcode=0"), reply("new boot")]):
        assert not R.check_respring(None, None, dev, result)
        assert "guest reset" in result.detail

print("PASS: respring requires readiness and preserves timeout diagnostics")
