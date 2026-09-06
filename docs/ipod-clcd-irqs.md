# CLCD frame interrupts on 7E18

AppleM2CLCD uses two separate registers: offset 0x08 enables interrupt sources;
offset 0x0c reports pending sources and acknowledges them by writing one bits.
Bit 0 is the frame event. The prior model ignored offset 0x08, returned a
constant 1 from offset 0x0c, and raised the frame IRQ whenever the last status
write was exactly 1. Acknowledging a frame therefore kept interrupts running
after the guest disabled them.

The 7E18 driver confirms the contract:

- c05e4bbc acknowledges bit 0, then c05e4bcc/c05e4bd0 adds bit 0 to the cached
  enable mask and writes it to offset 0x08.
- c05e4cf4/c05e4cfc removes bit 0 from that mask when the idle frame work ends.
- c05e4ef8 reads pending status; c05e4efc intersects it with the cached mask.
  A nonzero status with no enabled source reaches the unexpected-interrupt
  diagnostic at c05e515c.
- Shutdown disables sources at c05e4384. Underrun sources use the 0x1700 mask;
  acknowledging them must not accidentally clear a pending frame.

The model now latches frame status on vblank, drives the IRQ from pending AND
enabled sources, and clears only acknowledged status bits. Masked events can
remain pending without waking the CPU. Guest rendering and host scanout retain
the existing frame cadence.

LCD migration version 3 preserves both new registers. Version 2 snapshots recover
the enable register from the saved generic register bank and start with no
pending event; version 1 falls back to its old frame-enable behavior. The old
render field remains in the wire layout for compatibility.

`tests/ipod/test_lcd_irq.py` exercises masked frames, delayed enable, partial W1C,
mask removal and old/current restore under ASan/UBSan. Native evidence and the
broader acceptance status are recorded in `docs/plan-progress.md`.
