#!/usr/bin/env python3
"""iPad touch -> digitizer follows the guest's UI turn (panel=, issue #21).

UIKit turns its portrait UI a quarter into the panel only when the panel scans wider than tall; the digitizer
reports in that portrait frame. So on the shipped (turned) panel a host touch at panel (x, y) is digitizer
(1 - y, 1 - x) (y from the bottom), and on a square or taller panel= it is (x, 1 - y).

Mutation (named, must fail this test): drop ipad1_map_touch's unturned branch.
"""
from pathlib import Path
import re, subprocess, tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'hw/arm/ipad1.c').read_text()
flag = re.search(r'^static bool ipad1_ui_turned = true;$', source, re.M)
fn = re.search(r'^static void ipad1_map_touch\([^)]*\)\s*\{.*?^}', source, re.M | re.S)
assert flag and fn, 'ipad1_ui_turned / ipad1_map_touch'
code = '#include <assert.h>\n#include <stdbool.h>\n#include <stdio.h>\n#include <math.h>\n' + flag.group() + '\n' + fn.group() + r'''
static bool near(float a, float b) { return fabsf(a - b) < 1e-3f; }
int main(void)
{
    float fx, fy;
    ipad1_map_touch(8192, 24576, &fx, &fy);           /* panel (0.25, 0.75) on the shipped panel */
    assert(near(fx, 0.25f) && near(fy, 0.75f));        /* digitizer (1 - 0.75, 1 - 0.25) */
    ipad1_map_touch(0, 0, &fx, &fy);
    assert(near(fx, 1) && near(fy, 1));
    ipad1_ui_turned = false;                           /* a square or taller panel= */
    ipad1_map_touch(8192, 24576, &fx, &fy);
    assert(near(fx, 0.25f) && near(fy, 0.25f));        /* digitizer (0.25, 1 - 0.75) */
    ipad1_map_touch(0, 0, &fx, &fy);
    assert(near(fx, 0) && near(fy, 1));
    puts("PASS: iPad touches reach the digitizer in the guest UI's frame, turned or not");
}
'''
with tempfile.TemporaryDirectory(prefix='touch-panel-') as d:
    c, exe = Path(d) / 't.c', Path(d) / 't'
    c.write_text(code)
    subprocess.run(['clang', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all', str(c), '-o', str(exe)], check=True, timeout=60)
    subprocess.run([str(exe)], check=True, timeout=20)
