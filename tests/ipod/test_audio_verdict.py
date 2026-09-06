#!/usr/bin/env python3
"""Audio acceptance must reject silence, short playback and swapped channels."""
import tempfile
import wave
from pathlib import Path
from unittest.mock import patch
import numpy as np
import regress as R

rate = 44100
with tempfile.TemporaryDirectory() as directory:
    path = str(Path(directory) / 'audio.wav')
    for seconds, frequencies, expected in (
        (6, (440, 880), True), (2, (440, 880), False),
        (6, (880, 440), False), (6, (0, 0), False),
        (6, (440, 440), False), (6, (440, 900), False),
    ):
        times = np.arange(seconds * rate) / rate
        samples = np.stack([12000 * np.sin(2 * np.pi * frequency * times)
                            for frequency in frequencies], axis=1).astype('<i2')
        with wave.open(path, 'wb') as output:
            output.setparams((2, 2, rate, 0, 'NONE', 'not compressed'))
            output.writeframes(samples.tobytes())
        result = R.Result('audio')
        with patch.object(R, 'log'):
            assert R.verify_audio(path, result) is expected, result.detail
print('PASS: stereo audio verdict rejects silence, short clips, wrong tones and swapped channels')
