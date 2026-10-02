# DSIM software reset: hardware status independent of boot strategy

The actual retail and secure-development ROM→LLB→iBoot 7E18 controls both
stopped at iBoot `0ff0c218`. The instruction reads DSIM STATUS at `3d800000`,
shifts it by 11 and repeats until bit20 is set. Immediately before that loop,
stock iBoot writes 1 to DSIM SWRST (`3d800004`). Those addresses identify
read-only diagnostic observations; no guest instruction dispatch is added.

Primary S5L8720-specific source corroborates the hardware contract:
[OpeniBoot header](https://github.com/iDroid-Project/openiBoot/blob/866562fdb1cfd019bcd77885c80fbf0af65d5c15/plat-s5l8720/includes/hardware/mipi_dsim.h)
defines STATUS_SWRST bit20 and SWRST_RESET bit0; its
[driver](https://github.com/iDroid-Project/openiBoot/blob/866562fdb1cfd019bcd77885c80fbf0af65d5c15/plat-s5l8720/mipi_dsim.c)
writes SWRST and polls STATUS_SWRST, without a boot-strategy condition.

The model previously reported bit20 permanently only under direct-boot
compatibility. Now the device reset clears an explicit completion latch and
an actual SWRST bit0 command performs the existing synchronous FIFO/interrupt
reset and asserts release. STATUS is readonly; reads do not consume release.
Zero/other-bit SWRST writes cannot create it. Clock/lane status and the K48
`hs-clock-at-reset` compatibility setup are unchanged. Analog reset latency is
unmodeled; this does not certify PLL settling, ULPS or packet transmission.

The latch is serialized in DSIM VMState version3. Restoring version1/2 records
sets the previously unknown latch false; equivalent boot-stage completion is
not claimed for those older streams. The subsequent ULPS correction replaces those former compatibility command
bits with persistent D-PHY lane state; see the qualification below.

Verification: actual-source ASan/UBSan FIFO/reset suite passes, including both
compatibility states. Dedicated actual-board `ipod-dsim-test` passes4/4: cold
and direct reset/read-only/read-stability, warm reset, migration before and after
software reset. Durable logs are under
`/Users/shg/Developer/ltm-fidelity/evidence/ltm-n72-security-profile-candidate/`:
`dsim-build.log`, `dsim-board-test.log`, and `dsim-authentic-artifacts/receipt.json`.
The private artifact replaces only AES with pinned suppression-free f81b; no
native result is implied by its successful build. The native authentic ROM→LLB→iBoot control progressed past the original
reset poll to the subsequent ULPS poll at `0ff0c2c0` (durable
`secure-dev-dsim-positive/result.json`). Reset-only direct-iBoot baseline
`dsim-reset-direct-control` passes boot and offline filesystem checking, then
performs actual guest PMU shutdown with exit0. Neither result proves a complete
authentic chain to the kernel.


## D-PHY ULPS state

Stock iBoot writes ESCMODE `0x8a` and waits for STATUS `0x230`, then asserts
exit bits with `0x8f` and waits for ULPS status to clear. The S5L8720 primary
header defines clock enter/exit bits1/0, data enter/exit bits3/2, clock ULPS
STATUS bit9, and data ULPS bits starting at4. The driver uses the same sequence.
The former model supplied these bits as a read-consumed packet acknowledgement
only for direct boot. Packet writes cannot establish or consume D-PHY ULPS.

The model now stores ESCMODE and independent clock/data low-power states.
Enter requests act on rising edges; exit has priority. Consequently stock
`8a→8f→8a→80` exits once and cannot re-enter when the exit bits are removed.
STATUS reads are inert; software/device resets clear requests and state.
The connected physical lane mask remains the existing `lanes` property.
Dynamic CONFIG lane subsets, analog entry/exit delays and PLL settling are
not qualified by this correction. K48 kboot HS-clock initialization remains an
explicit compatibility mode; the four-lane board test is model coverage, not
an authentic K48 native boot qualification.

VMState version4 stores ESCMODE and clock/data states. The obsolete
`cmd_pending` uint32 remains only as a legacy wire slot, with no hardware
effect. Pre-v4 streams initialize the new states idle; equivalent legacy guest
resume behavior is unqualified. DSIM `direct_boot` and its board wiring are
removed because they no longer own hardware behavior.

Actual-source ASan/UBSan tests pass for one through four lanes, independent
enter/exit, packet separation, stable reads, reset, and stock request ordering.
Actual-board suite passes7/7, adding two-lane sequencing, four physical lanes,
and migration preserving exit-edge state. Logs: durable
`ltm-dsim-ulps-candidate/build.log` and `board-test.log`. Private authentic
artifact receipt: `ltm-n72-security-profile-candidate/ulps-authentic-artifacts/receipt.json`.
Its SHA256 is `9c28a755f369c616f5db6b6bfc26a150f04e73b8d55b1c227b5b524a387a1a74`.
Both native stock-chain controls now reach every precise checkpoint: ROM0,
LLB22000000, iBoot0ff00000, and the original decrypted stock kernel entry
08069040. Actual stock ROM verifier returns0 with CPFM1 secure-development and
CPFM3 retail, forge-sigcheck off, canonical NOR unchanged, and no direct loading
or emulator boot-argument writes. Durable results are
`secure-dev-ulps-positive/result.json` and `retail-ulps-positive/result.json`.
This qualifies the authentic handoff to the kernel, not stock userland UI,
restore durability, K48, analog timings, or the subsequent helper/app corpus.
ULPS direct-iBoot baseline and full userland regressions remain pending.
