#!/usr/bin/env python3
"""Exercise the production bounded trace without extra guest transactions."""
import ast
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'hw/arm/ipod_touch_fmss.c').read_text()
header = (ROOT / 'include/hw/arm/ipod_touch_fmss.h').read_text()
script = ast.parse((ROOT / 'tests/ipod/test_fmss_script.py').read_text())
prelude = next(ast.literal_eval(n.value) for n in script.body
               if isinstance(n, ast.Assign) and any(isinstance(t, ast.Name)
               and t.id == 'prelude' for t in n.targets))
prelude = prelude.replace('#define fmss_script_trace(...) ((void)0)', '')
prelude = prelude.replace('static unsigned descriptor_reads;',
                          'static unsigned descriptor_reads, reads, writes;')
prelude = prelude.replace('    assert(n == 4 || n == 8);',
                          '    reads++; assert(n == 4 || n == 8);')
prelude = prelude.replace('    if ((uint64_t)a + n > sizeof(mem)) return MEMTX_DECODE_ERROR;',
                          '    writes++; if ((uint64_t)a + n > sizeof(mem)) return MEMTX_DECODE_ERROR;')
prelude += '\n#include <stdlib.h>\nstatic bool enabled;\nstatic bool fmss_script_trace_on(void) { return enabled; }\n'
functions = []
for name in ('fmss_script_trace', 'fmss_var_read', 'fmss_run_script'):
    match = re.search(r'^static [^\n]*\b' + name + r'\([^)]*\)\s*\{.*?^}', source, re.M | re.S)
    assert match, name
    functions.append(match.group())
constants = '\n'.join(l for l in header.splitlines() if l.startswith('#define FMSS'))
constants += '\n' + '\n'.join(l for l in source.splitlines()
                              if l.startswith(('#define FMSS_CHIP', '#define FMSS_SCRIPT')))
main = r'''
int main(int argc, char **argv) {
    enabled = argc > 1;
    const uint32_t program[] = {
        0x05000000, 0xa000, 0x03010000, 0,
        0x02010804, 0, 0x01000008, 0x90,
        0x01000000, 2, 0x01000030, 7,
        0x01000014, 0x10, 0x01000004, 0xe2,
        0x01000040, 0x82, 0x04000060, 0xffffffff,
        0x05070000, 0x9000, 0x11000007, 0, 0, 0
    };
    IPodTouchFMSSState s = {.reg_cs_script = 0x1000,
                            .reg_csgenrc = argc > 2 ? 0xa02 : 0xa01};
    stl_le_p(mem + 0xa000, 0xabcddcba);
    memcpy(mem + 0x1000, program, sizeof(program));
    fmss_run_script(&s);
    assert(ldl_le_p(mem + 0x9000) == FMSS_CHIP_ID);
    printf("reads=%u writes=%u descriptor=%u id=%08x\n", reads, writes,
           descriptor_reads, ldl_le_p(mem + 0x9000));
    /* Stress the exact production cap; no extra model/guest access. */
    for (unsigned i = 0; i < FMSS_SCRIPT_TRACE_LIMIT + 9; i++) {
        fmss_script_trace("probe", 0x1000, i, 0, 0, s.reg_csgenrc);
    }
}
'''
with tempfile.TemporaryDirectory(prefix='fmss-script-trace-') as temp:
    path = Path(temp)
    (path / 'trace.c').write_text(prelude + constants + '\n' + '\n'.join(functions) + main)
    subprocess.run(['clang', '-std=c11', '-fsanitize=address,undefined',
                    '-fno-omit-frame-pointer', str(path / 'trace.c'), '-o', str(path / 'trace')], check=True)
    disabled = subprocess.run([str(path / 'trace')], capture_output=True, text=True, check=True)
    enabled = subprocess.run([str(path / 'trace'), 'enabled'], capture_output=True, text=True, check=True)
    assert not disabled.stderr, disabled.stderr
    assert enabled.stdout == disabled.stdout
    lines = enabled.stderr.splitlines()
    assert len(lines) == 16385, len(lines)
    assert lines[-1] == 'FMSS_SCRIPT_TRACE truncated limit=16384'
    assert sum('truncated' in l for l in lines) == 1
    assert any('fmc_write_unmodeled' in l and 'arg=00000804' in l for l in lines)
    assert any('descriptor_read' in l and 'arg=0000a000 value=00000004' in l for l in lines)
    assert any('store' in l and 'arg=00009000 value=00000004' in l for l in lines)
    assert not any('abcddcba' in l or 'b614d5ad' in l for l in lines)
    selected_env = os.environ | {'FMSS_SCRIPT_TRACE_CSGENRC': '0xa02'}
    ignored = subprocess.run([str(path / 'trace'), 'enabled'], env=selected_env,
                              capture_output=True, text=True, check=True)
    assert ignored.stdout == disabled.stdout and not ignored.stderr
    selected = subprocess.run([str(path / 'trace'), 'enabled', 'write'], env=selected_env,
                               capture_output=True, text=True, check=True)
    assert selected.stdout == disabled.stdout and selected.stderr == enabled.stderr
    for invalid in ('', '-1', '0xa02junk', '0x100000000', 'nonsense'):
        refused = subprocess.run([str(path / 'trace'), 'enabled'],
                                  env=os.environ | {'FMSS_SCRIPT_TRACE_CSGENRC': invalid},
                                  capture_output=True, text=True, check=True)
        assert refused.stdout == disabled.stdout
        assert refused.stderr == 'FMSS_SCRIPT_TRACE invalid CSGENRC selector; tracing disabled\n'
    print('PASS selector nonmatch silence, match parity, invalid single refusal; trace disabled silence, identical guest reads/writes/results, unsupported auxiliary access, payload exclusion, fixed cap and single overflow marker')
