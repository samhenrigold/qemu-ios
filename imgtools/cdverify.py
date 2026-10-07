#!/usr/bin/env python3
"""Verify a Mach-O's LC_CODE_SIGNATURE CodeDirectory page hashes match its code pages.

Confirms, offline, that after patching+resigning the per-page CD hashes agree with the
actual __TEXT bytes - i.e. the kernel's page-in hash check at 0x333ab000 will pass.
Prints the hash slot covering file offset 0x9ce4 (our patch) specifically.
"""
import hashlib, struct, sys

FAT_MAGIC=0xcafebabe
MH_MAGIC=0xfeedface
LC_CODE_SIGNATURE=0x1d
CSMAGIC_EMBEDDED_SIGNATURE=0xfade0cc0
CSMAGIC_CODEDIRECTORY=0xfade0c02
HASH_NAMES={1:"SHA1",2:"SHA256"}

def be(b,o,n): return int.from_bytes(b[o:o+n],'big')
def le(b,o,n): return int.from_bytes(b[o:o+n],'little')

def find_codesig(data):
    magic=le(data,0,4)
    assert magic==MH_MAGIC, "expected thin 32-bit LE Mach-O (magic 0x%08x)"%magic
    ncmds=le(data,16,4); off=28
    for _ in range(ncmds):
        cmd=le(data,off,4); sz=le(data,off+4,4)
        if cmd==LC_CODE_SIGNATURE:
            return le(data,off+8,4), le(data,off+12,4)  # dataoff, datasize
        off+=sz
    raise SystemExit("no LC_CODE_SIGNATURE")

def main():
    path=sys.argv[1]
    patch_off=int(sys.argv[2],0) if len(sys.argv)>2 else 0x9ce4
    data=open(path,'rb').read()
    cs_off,cs_sz=find_codesig(data)
    blob=data[cs_off:cs_off+cs_sz]
    assert be(blob,0,4)==CSMAGIC_EMBEDDED_SIGNATURE, "no SuperBlob"
    count=be(blob,8,4)
    cd_off=None
    for i in range(count):
        typ=be(blob,12+8*i,4); ofs=be(blob,12+8*i+4,4)
        if be(blob,ofs,4)==CSMAGIC_CODEDIRECTORY and cd_off is None:
            cd_off=ofs
    assert cd_off is not None, "no CodeDirectory"
    cd=blob[cd_off:]
    # CS_CodeDirectory fields (big-endian): magic0 len4 ver8 flags12 hashOffset16
    # identOffset20 nSpecialSlots24 nCodeSlots28 codeLimit32 hashSize36 hashType37
    # platform38 pageSize39
    hashOffset=be(cd,16,4); nCodeSlots=be(cd,28,4)
    codeLimit=be(cd,32,4); hashSize=cd[36]; hashType=cd[37]
    pageShift=cd[39]; pageSize=1<<pageShift if pageShift else 0
    algo={1:hashlib.sha1,2:hashlib.sha256}[hashType]
    print("CD: hashType=%s hashSize=%d pageSize=%d nCodeSlots=%d codeLimit=0x%x"
          %(HASH_NAMES.get(hashType,hashType),hashSize,pageSize,nCodeSlots,codeLimit))
    bad=0
    patch_slot=patch_off//pageSize
    for slot in range(nCodeSlots):
        start=slot*pageSize; end=min(start+pageSize,codeLimit)
        want=cd[hashOffset+slot*hashSize:hashOffset+(slot+1)*hashSize]
        got=algo(data[start:end]).digest()
        ok=(got==want)
        if not ok: bad+=1
        if slot==patch_slot or not ok:
            print("  slot %d [0x%x..0x%x] %s%s"%(slot,start,end,
                  "OK" if ok else "MISMATCH",
                  "  <-- patch page (off 0x%x)"%patch_off if slot==patch_slot else ""))
    print("RESULT: %d/%d slots match, %d mismatched"%(nCodeSlots-bad,nCodeSlots,bad))
    sys.exit(1 if bad else 0)

if __name__=="__main__":
    main()
