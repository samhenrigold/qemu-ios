# Frame-reference identity and provenance

The old top-level `ipad-{home,screen,lock}.png` pictures lack recorded firmware
identity and capture provenance. The home picture contains iOS 4's layout and
wallpaper; it is not a valid expected picture for fresh iPad 3.2.2 (7B500).
Keep these files as historical audit artifacts. The iPad regression gate never
falls back to them.

Qualified references belong under `<board>/<build>/`, for example:

```
k48ap/7B500/reference.json
k48ap/7B500/home.png
```

`reference.json` records exact build/product version and independent baseline
provenance, with hashes for both source capture and committed downsample:

```json
{
  "build": "7B500",
  "product_version": "3.2.2",
  "baseline": {
    "renderer": "stock-software-coreanimation",
    "provenance": "Record baseline QEMU commit/binary SHA, device input hashes, preparation commands, software-CA configuration, capture commands, and source artifact location here."
  },
  "frames": {
    "home": {
      "sha256": "SHA256 of home.png (64 lowercase hex characters)",
      "source_sha256": "SHA256 of original baseline screendump (64 lowercase hex characters)"
    }
  }
}
```

`stock-software-layerkit` and `physical-device` are also accepted renderers,
with corresponding stock configuration, device, capture method and source
provenance. Product version and build must come from
the tested device's lock, not an assumed harness default.

To qualify a new baseline:

1. Capture a separate known-good device running the same stock firmware, default
   wallpaper, icon arrangement and orientation. Use stock software CoreAnimation
   (`CA_ENABLE_OGL=0`) or LayerKit (`LK_ENABLE_OGL=0`) with evidence that
   the guest is using it, or a physical device. Disable automatic GL/MBX
   selection where applicable and verify that a previously installed frontend
   is actually restored to stock, rather than trusting its backup filename. For an emulator capture, record the independent baseline executable's
   commit/hash and the prepared device/input hashes. Candidate GL screenshots,
   including historical matrix candidate outputs, are not independent baselines.
2. Save the full source screendump with those artifacts. Review the picture and
   configuration before committing a reference; a manifest is provenance, not
   proof that a capture is correct.
3. Produce the small reference with `tests/framecheck.py make home.png SOURCE`.
   Record source/reference SHA-256 and exact firmware identity in the manifest.
4. Run the board reference tests (`tests/ipad1/test_frame_references.py` or
   `tests/ipod/test_frame_references.py`), then the real `gles` gate. Keep
   `framecheck`'s existing drift tolerance and differing-block threshold.

The dedicated iPad `gles` check requires a qualified `home` reference and FAILs
explicitly when one is absent or invalid. This is missing visual coverage,
not proof of a rendering defect. Boot/counter-only checks remain separately
useful and now state when no frame-reference comparison occurred.

The qualified `k48ap/7B500/home.png` reference comes from a separate fresh
software-CoreAnimation preparation, with live firmware identity and source
hashes in its manifest. Full lock/home captures are retained in the recorded
artifact directory; the clock-varying lock is not committed as a static golden.
Exact N45 references under `n45ap/3A101a` and `n45ap/4B1` now qualify the
three SpringBoard frontend scenes for1.1 and1.1.5. Their manifests identify
stock-software controls with zero GL contexts/hellos, source hashes and durable
evidence. Normal GL candidates pass strict Boot/GLES2/2 against those controls.
The1.1 Safari Wi-Fi dialog and1.1.5 Bookmarks state are retained as captured;
other legitimate UI states need their own matched expectations.

The shared validator requires exact board/build and a nonempty numeric full
product version. The iPod frontend gate requires all three qualified scenes;
missing coverage fails before output creation or guest launch. Old major-version
iPod pictures remain historical artifacts and are never borrowed for a different
build. Explicit GLTest app checks have their own scene oracle and remain distinct
from SpringBoard frontend qualification. Other builds still need independently
qualified references; candidate frames cannot provide their own oracle.

The iPod scene keys are `swipe`, `opened-app` and `home`. The second scene
is the stock app opened by the unchanged first-icon tap. This is Safari on
1.x/2.x and Mail on3.0; each manifest identifies the actual app. Historical
source captures may use the old `safari` filename. Their bytes and hashes are
preserved; relabeling the scene does not establish browser/network support.

Exact N72 references for `5F138` (2.1.1) and `7A341` (3.0) come from verified
original rootfs frameworks restored through guest filesystem operations, with
all six CA/LK software flags set and zero host GL contexts/hellos. The2.1.1
control's first clean reopen passed but its final software-rendered shutdown
failed; the manifest preserves this visual-only scope. The3.0 control ran
loader14 with legacy seed13 and passed both clean shutdowns/persistence. Its
userspace UART was unavailable; actual framework bytes and guest state provide
the restoration receipt. Neither control establishes physical GPU execution,
native cache-only hook rollback or full-catalog coverage.

`--gles-front-end` explicitly selects this gate even when helpers are installed;
it is mutually exclusive with `--gles-app`. Existing default GLTest selection
and app-fixture eligibility remain unchanged.
