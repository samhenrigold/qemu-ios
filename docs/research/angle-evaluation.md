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
- The compiled CGL renderer imports 196 GL functions. ANGLE directly exports
  152 of those names; 44 need a different name, an adapter, or replacement.
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

## Adoption decision and remaining acceptance

The probe proves native API viability, not guest compatibility or increased
stability. It does not replay the guest transport, test CoreAnimation surfaces,
PVRTC/palettes, background/foreground transitions, shared resources, snapshot
round trips, or the app corpus. No default change is justified by two triangles.

Keep the production CGL backend for this consolidation. The native probe is encouraging for GLES shader compatibility, but the API and snapshot inventory shows that switching now would add an adapter layer without removing most of the executor. No observed guest stability gain offsets that complexity yet.

A future production switch must preserve those behaviors and remove enough translation
code to offset the required adapters. Keep CGL as the measured baseline while
building that backend. Do not delete surface, transport or snapshot code simply
because ANGLE implements GLES; those are emulator responsibilities.

## Follow-up contract probe, 2026-09-30

The pinned build was reproduced on macOS 27.0.1 (26A434), Apple M4 Max.
The build script now resolves native Ninja before depot_tools changes PATH;
otherwise its Ninja wrapper failed with a missing Python bootstrap file even
though GN generation succeeded. No system libraries were installed.

`angle-probe.sh` now also exercises the context contracts used by the host:
a 1024x768 pbuffer, separate-group texture isolation, same-group texture
visibility, a shared texture surviving destruction of the root context, and
FBO pixel readback. These pass for ES1 and ES2. Creating an ES2 context sharing
with an ES1 context is accepted by this pin; cross-version resource and state
semantics still need guest coverage. The two original triangle probes pass.
The symbol audit now excludes the emulator's `gles_guest_*` functions: those
are host guest-memory helpers, not missing OpenGL entry points. The corrected
inventory is 196 imports, 152 direct exports, and 44 names needing adaptation.

The concrete replacement boundaries are:

| Contract | Required change | Responsibility retained |
|---|---|---|
| Context creation/lifetime | CGL handles, current-thread selection and root retention become EGL display/config/context lifetimes; supply an explicit guest ES version when creating a context | Opaque guest handles, sharegroup ownership and object identity |
| Executor API | Resolve OES/core FBO names per API, convert double-valued functions; replace desktop immediate drawing, attribute push/pop and APPLE fence behavior | Wire decoding, guest-pointer validation, caps and refusal/error semantics |
| Drawable/surface | EGL offscreen surface/context binding plus GLES FBO presentation/readback; prove accepted CA drawable sizes and all guest surface orders | IOSurface metadata, guest memory pitch/format, writeback and dirty tracking |
| Snapshots | Replace `glGetTexImage` with format-aware FBO/readback or retained uploads; capture API-specific ES state and rebuild sharegroups in dependency order | Persistent guest object names, surface state and migration ABI |
| Shader compilation | Submit GLES source directly to the ES backend after validating guest memory | Guest string/length validation, program object identity and shader snapshots |
| Acceptance | Run `regress.py` (qualified home frame, shadow, persistence), `snapshot-check.py`, `jank.py`, and the app corpus against two actual backend binaries | Existing thresholds and guest additions; no new golden frames derived from the candidate |

These probes use native EGL API calls, not the guest transport. There is no
second guest-capable backend binary to compare yet: linking the executor to
ANGLE leaves 44 unresolved names and its CGL context/snapshot assumptions.
A real comparative guest run requires a bounded experimental executor adapter
first. Inventing a second transport decoder for the probe would not validate
the production executor. This investigation therefore does not claim guest
compatibility, fewer production lines, or increased stability, and does not
change the default backend. It also cannot resolve the independent stock SGX
firmware barrier described in `sgx-native-init.md`.

Local build and evidence: `/private/tmp/ltm-angle-comparative`,
`/private/tmp/ltm-angle-contract.log`, and
`/private/tmp/ltm-angle-export-audit.json`.
