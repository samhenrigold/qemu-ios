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
not claimed for those older streams. The ULPS section below replaced the remaining compatibility command bits.

Verification: actual-source ASan/UBSan FIFO/reset suite passes, including both
compatibility states. Dedicated actual-board `ipod-dsim-test` passes4/4: cold
and direct reset/read-only/read-stability, warm reset, migration before and after
software reset. Durable logs are under
`~/Developer/ltm-fidelity/evidence/ltm-n72-security-profile-candidate/`:
`dsim-build.log`, `dsim-board-test.log`, and `dsim-authentic-artifacts/receipt.json`.
The private artifact replaces only AES with pinned suppression-free f81b; no
native result is implied by its successful build. The native authentic ROM→LLB→iBoot control progressed past the original
reset poll to the subsequent ULPS poll at `0ff0c2c0` (durable
`secure-dev-dsim-positive/result.json`). Reset-only direct-iBoot baseline
`dsim-reset-direct-control` passes boot and offline filesystem checking, then
performs actual guest PMU shutdown with exit0. Neither result proves a complete
authentic chain to the kernel.

## D-PHY ULPS state

Stock iBoot writes ESCMODE (`3d800014`) `0x8a` and waits for STATUS `0x230`, then writes `0x8f` and waits for the
ULPS bits to clear. OpeniBoot's S5L8720 header gives the bits: ESCMODE clock enter/exit 1/0, data enter/exit 3/2;
STATUS clock ULPS bit 9, data ULPS bits 4 up (one per lane). The model used to fake these as read-consumed
acknowledgments, set by any ESCMODE or packet write and only on direct boot, so a stock ROM→LLB→iBoot chain
hangs at iBoot `0ff0c2be` (one that gets that far: on this tree the ROM still stops earlier, because AES KEYLEN
reads as zero; making it read back turns iBoot's first UID operation into the encrypt it asks for, and the
direct-iBoot boot then fails to load its DeviceTree). The DSIM now keeps ESCMODE and the clock and data lane ULPS states: an enter bit acts on
its rising edge, exit wins, and STATUS reads are inert (`8a→8f→8a→80` exits once and stays out). Software and
device reset clear them; VMState version 4 carries them and older streams load idle. `direct_boot` is gone from
the DSIM and its boards. `tests/qtest/ipod-dsim-test` covers the sequence, four lanes and migration.
