#!/usr/bin/env python3
"""Run directly: native lock state and exact app identity drive launch verdicts."""
from types import SimpleNamespace
from unittest.mock import Mock, patch
import regress as R


def reply(text, rc=0):
    return SimpleNamespace(stdout=text, stderr="", returncode=rc)


dev = SimpleNamespace(dir="/unused", qmp=Mock())
with patch.object(R.time, "sleep"):
    for replies, expected, swipes in (
        ([reply("sblaunch: locked=0 passcode=0")], True, 0),
        ([reply("sblaunch: locked=1 passcode=0"),
          reply("sblaunch: locked=0 passcode=0")], True, 1),
        ([reply("sblaunch: locked=1 passcode=1")], False, 0),
        ([reply("sblaunch: locked=0 passcode=0", 1)], False, 0),
        ([reply("sblaunch: old helper")], False, 0),
    ):
        dev.qmp.reset_mock()
        with patch.object(R, "springboard", side_effect=replies):
            ok, _ = R.unlock(None, 22, dev, tries=1)
            assert ok is expected
            assert dev.qmp.swipe.call_count == swipes

    for text, rc, expected in (
        ("sblaunch: frontmost=org.example.App", 0, True),
        ("sblaunch: frontmost=org.example.App.Other", 0, False),
        ("sblaunch: frontmost=com.apple.springboard", 0, False),
        ("sblaunch: frontmost=org.example.App", 1, False),
    ):
        with patch.object(R, "springboard", return_value=reply(text, rc)):
            assert R.foreground_is(None, 22, "org.example.App") is expected

    with patch.object(R, "prepare_launcher", return_value=22), \
         patch.object(R, "ipa_bundle_id", return_value="org.example.App"), \
         patch.object(R, "unlock", return_value=(True, "unlocked")), \
         patch.object(R, "springboard", return_value=reply("launch accepted")), \
         patch.object(R, "to_png"), patch.object(R, "lit_count", return_value=(255, 300000)), \
         patch.object(R, "foreground_is", return_value=False), patch.object(R, "log"):
        result = R.Result("applaunch")
        assert not R.check_applaunch(SimpleNamespace(ipa="test.ipa"), None, dev, result)
        assert "not the foreground" in result.detail
print("Launch checks passed: no blind swipes; unrelated foreground cannot pass")
