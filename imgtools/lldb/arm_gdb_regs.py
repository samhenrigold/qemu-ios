# lldb target definition for an ARM debugserver that answers no qRegisterInfo (the 4.x and 5.x
# DeveloperDiskImages'): the classic GDB ARM `g` layout those debugservers use, as lldb_shim.py answers it for 3.x.
#   settings set plugin.process.gdb-remote.target-definition-file arm_gdb_regs.py
import os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lldb, lldb_shim

FORMATS = {"hex": lldb.eFormatHex, "float": lldb.eFormatFloat, "vector-uint8": lldb.eFormatVectorOfUInt8}
ENCODINGS = {"uint": lldb.eEncodingUint, "ieee754": lldb.eEncodingIEEE754, "vector": lldb.eEncodingVector}


def get_target_definition():
    sets, regs = [], []
    for line in lldb_shim.REGINFO:
        r = dict(f.split(":", 1) for f in line.rstrip(";").split(";"))
        if r["set"] not in sets:
            sets.append(r["set"])
        r["set"] = sets.index(r["set"])
        for k in ("bitsize", "offset", "gcc", "dwarf"):
            r[k] = int(r[k])
        r["format"] = FORMATS[r["format"]]       # lldb takes these as numbers; its names differ from the packet's
        r["encoding"] = ENCODINGS[r["encoding"]]
        regs.append(r)
    return {"sets": sets, "registers": regs, "host-info": {"triple": "armv7-apple-ios", "endian": "little"}}


def get_dynamic_setting(target, setting_name):
    if setting_name == "gdb-server-target-definition":
        return get_target_definition()
