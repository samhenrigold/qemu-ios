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
     patch.object(R, "prepare_app_control", return_value=R.AgentControl(None)), \
     patch.object(R.time, "sleep"), patch.object(R, "log"):
    dev = SimpleNamespace(dir=directory, qmp=Mock(reset_count=0))
    result = R.Result("respring")
    with patch.object(R, "spawn", side_effect=[reply()]) as stop, \
         patch.object(R, "springboard", side_effect=[reply(rc=124), reply("sblaunch: locked=1 passcode=0")]):
        assert R.check_respring(None, None, dev, result)
        assert stop.call_args.args[1] == ["/bin/launchctl", "stop", "com.apple.SpringBoard"]

    for response in (reply(rc=124), reply("unrelated successful reply")):
        result = R.Result("respring")
        with patch.object(R.time, "monotonic", side_effect=[0, 0, 1, 46]), \
             patch.object(R, "spawn", side_effect=[reply(), reply("launchd jobs")]), \
             patch.object(R, "springboard", side_effect=[response]), \
             patch.object(R, "guest_file", return_value=b"guest crash report"):
            assert not R.check_respring(None, None, dev, result)
            assert "did not recover" in result.detail
            assert "guest crash report" in Path(directory, "respring-diagnostics.txt").read_text()

    def status(*args):
        if dev.qmp.cmd.call_count > initial_calls + 1:
            dev.qmp.reset_count += 1
    initial_calls = dev.qmp.cmd.call_count
    dev.qmp.cmd.side_effect = status
    result = R.Result("respring")
    with patch.object(R, "spawn", side_effect=[reply(), reply("new boot")]), \
         patch.object(R, "springboard", side_effect=[reply("sblaunch: locked=0 passcode=0")]), \
         patch.object(R, "guest_file", return_value=b""):
        assert not R.check_respring(None, None, dev, result)
        assert "guest reset" in result.detail

print("PASS: respring requires readiness and preserves timeout diagnostics")
