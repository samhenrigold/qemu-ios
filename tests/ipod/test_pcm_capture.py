#!/usr/bin/env python3
"""Find a reference clip in an I2S capture and compare every stereo sample.

Both inputs are stereo at the same sample rate and unity volume. The reference
is S16LE. Capture defaults to S16LE; --coreaudio reads the native F32LE tap.
Requires NumPy for FFT alignment. Allows one quantization step of difference.
"""
import argparse
import numpy as np


def check(capture_path, reference_path, coreaudio=False, skip_frames=0, take_frames=None):
    assert skip_frames >= 0 and (take_frames is None or take_frames > 0)
    dtype = np.dtype('<f4' if coreaudio else '<i2')
    capture = np.fromfile(capture_path, dtype=dtype,
                          offset=skip_frames * 2 * dtype.itemsize,
                          count=-1 if take_frames is None else take_frames * 2)
    reference = np.fromfile(reference_path, dtype='<i2')
    assert reference.size and reference.size % 2 == 0, 'incomplete reference frame'
    capture = capture[:capture.size // 2 * 2].reshape(-1, 2).astype(np.float64)
    assert np.all(np.isfinite(capture)), 'capture contains invalid samples'
    if coreaudio:
        # FLOAT_MIXENG's signed S16 conversion uses (INT16_MAX-INT16_MIN)/2.
        capture = np.rint(capture * 32767.5)
    reference = reference.reshape(-1, 2).astype(np.float64)
    assert len(capture) >= len(reference), 'capture is shorter than reference'
    assert np.max(np.abs(reference)) > 0, 'silent reference cannot verify audio'
    channel = int(np.argmax(np.sum(reference * reference, axis=0)))
    a, b = capture[:, channel], reference[:, channel]
    length = 1 << (len(a) + len(b) - 1).bit_length()
    corr = np.fft.irfft(np.fft.rfft(a, length) * np.fft.rfft(b[::-1], length), length)
    valid = corr[len(b)-1:len(a)]
    offset = int(np.argmax(valid))
    actual = capture[offset:offset+len(reference)]
    difference = np.abs(actual-reference)
    worst = int(difference.max())
    assert worst <= 1, f'maximum sample error {worst} at offset {offset}'
    print(f'PASS: {len(reference)} stereo frames, offset {offset}, maximum sample error {worst}')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture_path')
    parser.add_argument('reference_path')
    parser.add_argument('--coreaudio', action='store_true')
    parser.add_argument('--skip-frames', type=int, default=0)
    parser.add_argument('--take-frames', type=int)
    check(**vars(parser.parse_args()))
