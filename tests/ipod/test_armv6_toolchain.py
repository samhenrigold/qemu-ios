#!/usr/bin/env python3
"""A failed guest-helper compile must not silently reuse an old object."""
import pathlib, subprocess, tempfile
root = pathlib.Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory() as tmp:
    tmp = pathlib.Path(tmp)
    src, obj = tmp/'input.c', tmp/'output.o'
    src.write_text('this is deliberately invalid C;\n')
    obj.write_bytes(b'stale object')
    args = ['bash', '-c', 'source "$1"; cc6 "$2" "$3" -DCHECK_VALUE=7', 'check',
            str(root/'contrib/armv6-toolchain/armv6.sh'), str(src), str(obj)]
    result = subprocess.run(args, capture_output=True, text=True)
    assert result.returncode != 0 and not obj.exists(), result
    src.write_text('int check(void) { return CHECK_VALUE; }\n')
    result = subprocess.run(args, capture_output=True, text=True)
    assert result.returncode == 0 and obj.stat().st_size > 0, result.stderr
    assert not list(tmp.glob('*.cclog'))
print('PASS: failed ARMv6 compiles reject stale objects; extra compiler flags are forwarded')
