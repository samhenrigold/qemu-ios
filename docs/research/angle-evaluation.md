# ANGLE Metal evaluation, 2026-09-30

The authorized consolidation work revisits the earlier decision to drop ANGLE.
This checkout now contains a pinned isolated build script, a rendering probe,
and an inventory of the compiled renderer's host API dependencies. ANGLE is
not yet the production renderer.

## Reproduce

```
bash scripts/build-angle-prototype.sh /tmp/new-angle-evaluation
bash tests/ipad1/angle-probe.sh /tmp/new-angle-evaluation/angle
python3 tests/ipad1/angle-export-audit.py \
  build-fidelity/libqemu-arm-softmmu.a.p/hw_arm_gles-host.c.o \
  /tmp/new-angle-evaluation/angle/out/ltm-metal/libGLESv2.dylib
```

ANGLE is pinned to `8cd050f07ebd65cce269fc92e36699fc03b20ac3`; depot_tools
is pinned to `9b264039190fa270f3fc44779b4e26b43008ba41`. Upstream DEPS
selects the remaining dependencies. The script downloads an isolated tree;
it does not install or replace system libraries. Ninja is invoked directly:
this depot_tools checkout's autoninja expected a different Python bootstrap.
The tested component build occupies about 8.7 GB including source and tools.
Its six dylibs total about 11.6 MB before packaging/signing.

## Observed

On Apple M4 Max, macOS 27.0 (26A428), Metal-only release ANGLE:

- ES1 fixed-function triangle: center pixel RGBA `255,0,0,255`.
- ES2 triangle with the original precision-qualified GLSL: center pixel RGBA
  `0,255,0,255`; both shaders compile and the program links without our GLSL
  text rewriting. The context requested version 2; ANGLE advertises ES3.0.
- EGL context/surface creation, switching, destruction and readback succeed.
- The compiled CGL renderer imports 198 GL functions. ANGLE directly exports
  152 of those names; 46 need a different name, an adapter, or replacement.
  This is a symbol inventory, not an unsupported-feature count.

The production executor is 7,801 lines plus 1,225 lines of snapshot support.
Most of it implements guest memory, object identity, surfaces, transport,
iOS-specific formats and snapshots. Those responsibilities remain with ANGLE.
Framebuffer suffixes and double-valued ES entry points are straightforward
renames/conversions. Immediate drawing, attribute push/pop, texture readback
and Apple fences are actual adaptation work. Existing snapshot capture also
assumes desktop texture readback and mutable fixed-function state.

There is another boundary to establish before an ANGLE backend: the current
wire operation creating a context supplies a sharegroup but no ES API version.
The public guest front end knows the version, while the CGL host currently
selects programmable draws from the active program. An EGL backend needs the
version when creating the context, plus the version in its snapshot state.

## Acceptance still required

The probe proves native API viability, not guest compatibility or increased
stability. It does not replay the guest transport, test CoreAnimation surfaces,
PVRTC/palettes, background/foreground transitions, shared resources, snapshot
round trips, or the app corpus. No default change is justified by two triangles.

A production switch must preserve those behaviors and remove enough translation
code to offset the required adapters. Keep CGL as the measured baseline while
building that backend. Do not delete surface, transport or snapshot code simply
because ANGLE implements GLES; those are emulator responsibilities.
