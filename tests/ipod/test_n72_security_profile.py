#!/usr/bin/env python3
"""Run actual production fuse transformation; secure is not security-domain."""
from pathlib import Path
import re, shutil, subprocess, tempfile
ROOT = Path(__file__).resolve().parents[2]
src = (ROOT / 'hw/arm/ipod_touch_chipid.c').read_text()
header = (ROOT / 'include/hw/arm/ipod_touch_chipid.h').read_text()
match = re.search(r'void ipod_touch_chipid_set_n72_profile\([^;]*?\)\s*\{', src)
assert match
end = match.end(); depth = 1
while depth:
    depth += (src[end] == '{') - (src[end] == '}'); end += 1
function = src[match.start():end]
enum = re.search(r'typedef enum \{.*?\} N72SecurityProfile;', header, re.S).group()
assert 'getenv(' not in src
body = """#include <stdint.h>
#include <assert.h>
#define g_assert_not_reached() assert(0)
typedef struct { uint32_t word1, word2, word3, word4; } IPodTouchChipIDState;
""" + enum + function + """
int main(void) {
 for(unsigned domain=0;domain<4;domain++) {
  for(unsigned oscillator=0;oscillator<2;oscillator++) {
   for(unsigned info_secure=0;info_secure<2;info_secure++) {
    for(unsigned prod_input=0;prod_input<2;prod_input++) {
    IPodTouchChipIDState original={0xabcd0000|(prod_input<<5),0x87200000|(domain<<2)|oscillator|(info_secure<<1),123,456};
    for(unsigned profile=0;profile<3;profile++) {
     IPodTouchChipIDState s=original;
     ipod_touch_chipid_set_n72_profile(&s,profile);
     unsigned production=(s.word1>>5)&1;
     unsigned secure=((s.word2>>1)&1)|production;
     assert((secure|(production<<1))==(profile==0?((info_secure|prod_input)|(prod_input<<1)):profile==1?1:0));
     if(profile==0) assert(s.word1==original.word1&&s.word2==original.word2);
     assert((s.word2&~2u)==(original.word2&~2u));
     assert((s.word1&~32u)==(original.word1&~32u));
     assert(s.word3==123&&s.word4==456);
     IPodTouchChipIDState once=s;
     ipod_touch_chipid_set_n72_profile(&s,profile);
     assert(s.word1==once.word1&&s.word2==once.word2);
    }
    }
   }
  }
 }
 return 0;
}
"""
with tempfile.TemporaryDirectory(prefix='n72-security-') as directory:
    c=Path(directory)/'fuses.c'; binary=Path(directory)/'fuses'; c.write_text(body)
    subprocess.run([shutil.which('clang') or 'cc','-std=c11','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined',str(c),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
print('PASS actual N72 CPFM03/01/00 transformation: SDOM/oscillator/identity preserved')
