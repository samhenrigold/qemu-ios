# N45 touch interrupt mask contract

The actual board qtest `ipod-touch-irq-test` sends a QMP touch through the
machine's real input handler, digitizer, SYSIC and VIC. No firmware executes;
zero images satisfy the board's input requirements. Before the correction,
GPIO_INTEN group 4 was zero but the VIC raw interrupt bit 2 was asserted.
The digitizer wrote SYSIC's pending latch and raised the group output directly,
bypassing the controller's `pending & enabled` calculation.

The digitizer now requests an edge through SYSIC. Pending remains latched while
masked; enabling the source presents it; disabling it lowers the shared output;
W1C acknowledgment clears it, and the next report creates a new pending edge.
The actual qtest checks all these transitions. This changes no guest code,
firmware command responses or guessed boot delay/readiness policy. Existing
button/I2S direct producers remain separate audit work.

Evidence: `/private/tmp/ltm-touch-qtest-before.log` fails at masked VIC output
(`0x4` instead of zero); `/private/tmp/ltm-touch-qtest-after.log` passes.

## Native handshake observations

Private NOR copies and overlays were used for prepared 3A101a and fresh 4B1.
Before the correction, continuous contact already completed both stock
AppleMultitouchZ2SPI download sequences without reproducing the previously
reported panic. The older trace issues HBPP `0x30`, register `0x1e`, calibration
`0x1f`, execute `0x1d`, then interface query `0xe2`. For 4B1, contact interrupts
occurred throughout the calibration interval, and execute/interface queries
completed at virtual time 3.663/3.666s. The downloaded firmware is 49128 bytes.
These observations justify tracing the protocol, not inventing a readiness
transition or declaring the historical panic fixed.

Before evidence: `/private/tmp/ltm-n45-early-hold-before` (3A101a),
`/private/tmp/ltm-n45-4B1-early-before` (4B1). Fresh 4B1 preparation:
`/private/tmp/ltm-n45-touch-4B1-create.log`. After correction, bounded continuous
contact probes are recorded at `/private/tmp/ltm-n45-early-hold-after` and
`/private/tmp/ltm-n45-4B1-early-after`; both complete stock downloads, mount
`disk0s1` and serve touch frame reads without desynchronization. This is bounded
regression evidence, not proof of all power transitions or historical panic
reproduction. Keep the harness home-screen stimulus restriction until the
specific old failure can be reproduced and explained.

N45 has no timer-written boot arguments in its machine. The recipe supplies
`boot-args` in NOR CHRP NVRAM, and stock iBoot builds the handoff. Both probes
show early kernel driver logging and `BSD root: disk0s1`; that is consistent with
the prepared arguments, but an independent handoff-buffer/NVRAM mutation probe
would be needed to prove each argument's consumption. N72's separate memory
injection compatibility remains open.

After-correction screenshots were inspected: 3A101a is at the home screen
(134434 lit subpixels); 4B1 is at the home screen with its expected Edit Home
Screen tutorial after the long contact (292322 lit subpixels). Both 75-second
probes have no guest panic or unknown-command desynchronization. This establishes
older/newer boot and functional touch regression for this specific mask change.
