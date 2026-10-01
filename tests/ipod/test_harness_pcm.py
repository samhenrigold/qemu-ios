#!/usr/bin/env python3
"""Compile the maintained PCM ownership doubles under ASan/UBSan; no SDK needed."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='ltm-harness-pcm-') as directory:
    binary = Path(directory) / 'pcm-test'
    subprocess.run(['xcrun', 'clang', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-parameter', '-Wno-unused-function',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    str(ROOT / 'contrib/it-harness/test-pcm.c'), '-o', str(binary)],
                   check=True, timeout=60)
    env = dict(os.environ)
    env['ASAN_OPTIONS'] = 'halt_on_error=1'
    env['UBSAN_OPTIONS'] = 'halt_on_error=1:print_stacktrace=1'
    subprocess.run([str(binary)], check=True, timeout=10, env=env)
print('PASS maintained Harness PCM ownership under ASan/UBSan; guest waveform qualification is separate')
