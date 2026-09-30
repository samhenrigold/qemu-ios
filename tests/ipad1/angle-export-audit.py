#!/usr/bin/env python3
"""Inventory compiled renderer dependencies absent from ANGLE; not a coverage test."""
import argparse
import json
import re
import subprocess
from pathlib import Path
ap=argparse.ArgumentParser(description=__doc__)
ap.add_argument('renderer_object',type=Path)
ap.add_argument('angle_library',type=Path)
a=ap.parse_args()
def symbols(argv):
    return set(re.findall(r'\b_(gl\w+)\s*$',subprocess.check_output(argv,text=True),re.M))
used=symbols(['nm','-u',str(a.renderer_object)])
exports=symbols(['nm','-gU',str(a.angle_library)])
assert used and exports,'missing compiled GL symbol inventory'
print(json.dumps({'renderer_imports':len(used),'direct_exports':len(used&exports),'unavailable_names':sorted(used-exports)},indent=2))
