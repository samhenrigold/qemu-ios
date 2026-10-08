# Physical security profiles and authentic boot qualification

The N72 board now accepts the immutable `security-profile` machine input:

| Profile | Production (+4 bit5) | Secure input (+8 bit1) | CPFM with default fuses |
|---|---:|---:|---:|
| `retail` (default) | preserved, default 1 | preserved, default 0 | 03 |
| `secure-development` | 0 | 1 | 01 |
| `insecure-development` (research control) | 0 | 0 | 00 |

This is a typed board configuration applied before ChipID realization. The retail
profile preserves existing physical fuse overrides. Nonretail profiles change only
the two specified bits; chip identity, unit ECID, oscillator selection and security
domain remain unchanged. Guest fuse writes, reset and post-start QOM changes cannot
change the profile. N45 continues to use its own fuse layout and board configuration.

The removed `IT_DEV_MODE`/`IT_INSECURE_MODE` environment reads had inaccurate
semantics: clearing production with the default INFO word produces CPFM00, while
clearing INFO bit2 changes SDOM1 to SDOM0. Neither operation described secure
engineering hardware correctly.

## Stock ROM and certificate evidence

The unmodified N72 ROM 240.4 SHA256 is
`55f4d8ea2791ba51dd89934168f38f0fb21ce8762ff614c1e742407c0d3ca054`.
Offsets here describe diagnostic source evidence, not emulator guest-code dispatch:

- `3d44`: reads CHIPID +4 bit5 (production).
- `3d54`: reads CHIPID +8 bit1 (secure input).
- `3d64`: reads CHIPID +8 bits3:2 (SDOM).
- `31e6`: secure is secure input OR production; `3264` returns
  secure | production << 1.
- Certificate admission checks SDOM at `21fe` in both production and development.
  The production check at `221e`/`2224` skips the PROD constraint for development,
  continuing at `2242`. It does not remove RSA or certificate-chain verification.

The original 7E18 IPSW production LLB public certificate chain contains Apple Secure
Boot CA and S5L8720 Secure Boot. Its critical Apple extension
`1.2.840.113635.100.6.1.1` contains SDOM1, PROD1 and CHIP8720 constraints. Therefore
secure engineering fuses admit the stock production constraint through the observed
ROM branch without requiring a different private signing key. This observation
establishes policy eligibility; it does not prove the native cryptographic chain.
Unmodified Apple-signed images contain the required public certificates and signatures.
An independent public-key check recovers a valid RSA PKCS#1 v1.5 SHA1 block
from the original LLB SHSH under its stock leaf public key. Its SHA1 digest
`e614580061042f0611cda321b53e26ed681f4eb1` matches the signed Img3 span
[12, 65804), where the end is 20 + sigCheckArea. Changing one SHSH byte fails
that same mathematical check. This establishes the original image signature,
not a native ROM execution or ROM trust-anchor qualification. The stock leaf
certificate signature also verifies under the included Apple Secure Boot CA.
All thirteen original 7E18 all_flash Img3 SHSH signatures independently match
their signed spans, including iBoot and DeviceTree; metadata is retained in
`~/Developer/ltm-fidelity/evidence/ltm-evm-startup-next/n72-stock-allflash-signatures.json`.

Creating new Apple-trusted signatures requires private signing material unavailable
to this project; catalog GID decryption keys are a separate issue.

Reproducible local diagnostic metadata (no private keys or firmware payload exports):
`~/Developer/ltm-fidelity/evidence/ltm-evm-startup-next/n72_fuse_cert_proof.py` and
`n72-fuse-cert-proof.json`. Source model test
`tests/slice/ipod-n72-security-profile.c` exercises actual production transformation
under ASan/UBSan across all SDOM values and both oscillator/secure-input values.
The actual-board `ipod-chipid-test` adds CPFM03/01/00, readonly writes, warm reset
and QOM immutability. Root rebuilt the actual board suite and all four cases
passed; durable evidence is `ltm-n72-security-profile-candidate/board-test.log`.
Native qualification is pending.

## Authentic qualification gate

Use `security-profile=secure-development,forge-sigcheck=off`, unmodified ROM and
original stock signed LLB/iBoot/kernel components, per-IPSW catalog decryption data,
and an isolated disposable store. Do not use direct-iBoot/direct-LLB shortcuts or
modify guest code. Pair successful full-chain boot with a second run that changes
one SHSH signature byte while preserving image structure; rejection must occur at
the actual stock verification boundary. A compatibility PKE success report cannot
qualify an authentic chain. Retail controls remain available and the preferred boot
route stays unchanged until the complete gate passes.

## K48 comparison

The physical K48 ROM 574.4 SHA256 is
`4f34652a238a57ae0018b6e66c20a240cdbee8b4cca59a99407d09f83ea8082d`.
Its ChipID base is `bf500000`, with production at word0 bit0, secure input bit1,
and SDOM bits3:2. ROM getters `53a8`, `53b8`, `53c8`, wrapper `949c`, and CPFM
assembly `83a0` show the same secure OR production formula in a different layout.
Certificate admission checks SDOM at `45ba`; `45de`/`45e2` skips PROD when
production is clear. Metadata is in `k48-fuse-cert-proof.json` beside the N72 proof.

Existing `development-fuses` clears word0 bits0 and7. Bit7 has its own getter
`53f8`; it is independent of CPFM and SDOM. A narrow secure-development profile
would clear only bit0, giving retail word0 `31800387` -> `31800386`, preserving
bit7 and all other identity. The existing broader mode gives `31800306`. This
comparison is source evidence only; no K48 production behavior changes here and
no complete authentic boot/signature-negative gate has been established.

## Snapshot compatibility

ChipID now records its four fuse inputs using QEMU equal-only migration fields.
A new snapshot with different production, secure, SDOM, oscillator or identity
fuses must be refused rather than overwriting destination hardware configuration.
Actual-board acceptance/refusal tests pass for N72 and N45; the rebuilt
ChipID suite passes8/8 (`migration-board-test.log` in the durable candidate folder). Snapshots predating the ChipID migration section
are not protected by its comparison. Direct-QEMU cross-profile restore of such
older snapshots remains unqualified; no host security-profile recipe or admission
claim is introduced here.

## Stock kernel provenance for the first native gate

Read-only inspection of the canonical generated-NAND fixture finds
`kernelcache.s5l8720x` byte-identical to the original 7E18 IPSW Img3 (3,952,004
bytes, SHA256 `6e13108ce57a443aa6654d4faa96b53ac353f591b7cb6777f3beabe334c220d0`).
The decrypted/LZSS-expanded stock Mach-O SHA256 is
`8caf1738b15fe4df99ddebb1f976b8582364af27ef738b93dcac60be1b26c63e`.
Its LC_UNIXTHREAD PC is `c0069040`; with __TEXT base `c0000000`, iBoot's
MMU-off physical entry is `08069040`, using the same conversion as the existing
keybag diagnostic. The bounded native probe can use `--kernel-pc 0x08069040`.
The filesystem contains userland additions, but this kernel and the NOR signed
spans remain original. Durable reproducible metadata is
`~/Developer/ltm-fidelity/evidence/ltm-n72-security-profile-candidate/kernel-proof.json`.
A prepared breakpoint recipe is not proof that the native chain reaches it.

## Actual stock verification controls

The native secure-development and retail controls both execute unmodified
ROM→LLB→iBoot with `forge-sigcheck=off`; the initial kernel handoff is blocked
by independently observed DSIM initialization state. The stock ROM image-verifier
call at diagnostic PC `1dea` and immediate return at `1dee` were captured with
read-only breakpoints. The original LLB returns R0=0. A private NOR with exactly
one changed LLB SHSH byte returns R0=`ffffffff`, with the same signed digest
and altered signature hash. This proves actual ROM crypto refusal; the earlier
timeout in its subsequent RAM-flag wait alone did not establish rejection.

Durable receipts: `~/Developer/ltm-fidelity/evidence/secure-dev-dsim-positive/result.json`
and `~/Developer/ltm-fidelity/evidence/secure-dev-verify-bad-retry/result.json`.
The profile remains opt-in. Complete ROM→LLB→iBoot→kernel, stock restore and
durable restored boot remain pending; no preferred host boot route changes.
