# Accelerometer controls

Light Touch's Motion toolbar menu selects Upright or Flat and the keyboard tilt
speed (45, 90 or 180 degrees/second). Hold Option and an arrow key to tilt;
release to return to rest. Option-Space shakes once per press. Keyboard tilt is
limited to 45 degrees. Dragging the chassis controls roll; a two-finger scroll
off the display controls roll and pitch. Sleep, loss of window focus and Live
Text cancel held motion input. The displayed shell follows pitch with a
perspective transform.

The mounted gravity calculation is shared by discrete orientation, continuous
attitude and the embedded frontend. `accel-pose` accepts `upright` or `flat`;
`accel-pitch` and `accel-roll` accept degrees from -180 through 180. Positive
pitch tips the top away; positive roll tips the right edge down. Raw sensor X
has the board's mounting inversion. `accel-x/y/z` override the requested raw
vector and read it back without sample noise.

I2C output registers sample that vector using virtual time, so rapid host
updates cannot produce extra sensor samples. The default follows CTRL_REG1's
DR bit (100/400 Hz). `accel-rate-hz=1..400` overrides it; zero returns to automatic
rate selection. Sampling is lazy on output-register reads; no idle host timer
is needed. Data-ready interrupts and STATUS overrun accounting are not modeled.

A nonzero gravity vector receives at most one count of noise per axis per
sample, saturated at the signed-byte limits. A zero vector remains zero. Noise
uses a device-local deterministic PRNG. Shake is a 200 ms alternating three-axis
impulse, followed by the current resting vector. Snapshot v3 saves the rate and
PRNG state; restoration cancels transient shake and begins a new sample period.
Versions 1 and 2 restore with automatic sample rate.

Set `IT_ACCEL_TRACE=1` to report the mean interval between guest OUT_X polls,
at most once per virtual second. `IPOD_ACCEL_DEBUG=1` enables individual sensor
register diagnostics. Both are off by default.

Focused checks: `tests/ipod/test_attitude.py`, `test_accel_sampling.py`,
`test_attitude_qmp.py`, and `test_accel_guest.py`. The guest check installs the
bundled Harness on an isolated overlay and reads UIKit's callback values through
the guest agent. Game-specific steering acceptance remains separate.

On 7E18, UIKit reports raw X/Y with their stored sign and raw Z inverted, at
about 18 mg/count: the model's flat rest reaches UIKit near (0, 0, +1.15).
A 20-degree pitch gives Y near +0.40 and a 20-degree roll gives X near -0.40.
This is measured guest behavior, not acceptance of face-up game controls; the
physical-pose conventions must be checked against the steering games before
claiming that mapping is complete.
