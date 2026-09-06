# PKE operation model

The S5L8720 PKE is a Montgomery arithmetic engine. Firmware performs RSA by
issuing a sequence of modular products; START is not a complete RSA operation.
The former fifth-START exponentiation with a fixed exponent has been removed.

| Offset | Model |
| --- | --- |
| `0x000` | Key-length/precision configuration, readable |
| `0x008` | Bit 3 loads modulus; bit 0 executes; reads zero after synchronous completion |
| `0x00c` | Source A, source B, modulus and destination segment indices, high byte first |
| `0x010` | Segment sign bits; results are canonical positive residues |
| `0x014` | Bits 7:6 select 256/128/64-byte segments; bit 1 selects A×1 instead of A×B |
| `0x024` | Software reset invalidates the preloaded modulus and clears signs |
| `0x800–0xfff` | 2 KB of little-endian operand SRAM |

Each execution computes `A × B × R⁻¹ mod M`. With precision
`p = (KEY_LEN & 3) + 1` and chunk width
`c = (((KEY_LEN >> 3) & 15) + 1) × 32`, the radix is `R = 2^(p × (c + 16))`.
A×1 ignores source B. Modulus loading copies the selected SRAM segment, so
later writes to that segment do not change the loaded modulus. Invalid segment
selection, absent/even modulus and arithmetic failures do not publish a result.

This protocol was checked against the local S5L8900X PKE driver reference in
`ipod2g-re/iBoot-master-*/drivers/samsung/pke/`, then against native 5F138 and
7E18 register traces. The first 5F138 operation uses KEY_LEN `0x7b`, 256-byte
segments and source/destination `5,5→6`; its initial operand is `2^2145 mod M`.
Returning the correct key-length register is essential: the previous zero read
changed the guest's number of precomputation iterations. The previous 1 KB SRAM
also dropped the higher temporary operands.

The explicit `IT_FORGE_SIGCHECK` compatibility mode remains scoped to the boot
verifier's final A×1 conversion from segment 2 into segment 1. It preserves valid
SHA1 DigestInfo blocks and substitutes only malformed recoveries. It is not
part of the arithmetic model and remains disabled by default.

Migration version 2 includes full SRAM, register selection, signs and the loaded
modulus. Version 1 cannot represent those values and is rejected. Light Touch's
existing build-identity snapshot gate also prevents cross-build reuse. Timing
and interrupt delivery are not modelled; both tested boot drivers poll START.

Checks:

- `python3 tests/ipod/test_pke.py`: production handlers with ASan/UBSan,
  512/1024/2048-bit RSA, arbitrary exponents, compatibility mode, short results,
  invalid commands, full SRAM bounds and saved-state validation.
- `python3 tests/ipod/test_pke_snapshot.py`: native paused-machine migration,
  with high SRAM segments, signs and a loaded modulus different from SRAM.
- Native 5F138 reaches Home; 7E18 passes firmware/agent acceptance and confirmed
  guest shutdown. Temporary native evidence is recorded in the plan tracker.
