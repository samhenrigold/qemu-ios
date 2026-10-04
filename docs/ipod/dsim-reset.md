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
not claimed for those older streams. Other compatibility command bits remain gated and read-consumed; actual
command execution is unqualified pending the separate ULPS correction.

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
