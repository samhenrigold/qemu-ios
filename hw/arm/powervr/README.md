# PowerVR PVRTC decoder

`PVRTDecompress.cpp`, `PVRTDecompress.h`, and `LICENSE.md` are unmodified from
Imagination Technologies' Native SDK revision
`34f68e4c028e06704ff2f69f292a61f9ba0c53c4`:
https://github.com/powervr-graphics/Native_SDK/tree/34f68e4c028e06704ff2f69f292a61f9ba0c53c4

Only `pvrtc.cpp` is built. It includes the vendor implementation and supplies a
C interface for validated guest uploads. The wrapper handles little-endian,
possibly unaligned input, compact legacy mip tails, and RGB's opaque alpha.
The vendor's ETC function is unused; no SDK runtime or frameworks are required.

`tests/ipod/test_pvrtc.py` uses independent golden hashes recorded before this
integration, checks modulation modes and rectangular grids, and runs with
ASan/UBSan. Upload tests exercise guest-byte validation and the actual CGL path.
