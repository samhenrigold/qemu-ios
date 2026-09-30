# Frame-reference identity and provenance

The old top-level `ipad-{home,screen,lock}.png` pictures lack recorded firmware
identity and capture provenance. The home picture contains iOS 4's layout and
wallpaper; it is not a valid expected picture for fresh iPad 3.2.2 (7B500).
Keep these files as historical audit artifacts. The iPad regression gate never
falls back to them.

A qualified iPad reference belongs under `k48ap/<build>/`:

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

`physical-device` is the other accepted renderer, with corresponding device,
capture method and source provenance. Product version and build must come from
the tested device's lock, not an assumed harness default.

To qualify a new baseline:

1. Capture a separate known-good device running the same stock firmware, default
   wallpaper, icon arrangement and orientation. Use stock software CoreAnimation
   (`CA_ENABLE_OGL=0`) with evidence that the guest is using it, or a physical
   device. For an emulator capture, record the independent baseline executable's
   commit/hash and the prepared device/input hashes. Candidate GL screenshots,
   including historical matrix candidate outputs, are not independent baselines.
2. Save the full source screendump with those artifacts. Review the picture and
   configuration before committing a reference; a manifest is provenance, not
   proof that a capture is correct.
3. Produce the small reference with `tests/framecheck.py make home.png SOURCE`.
   Record source/reference SHA-256 and exact firmware identity in the manifest.
4. Run `tests/ipad1/test_frame_references.py`, then the real `gles` gate. Keep
   `framecheck`'s existing drift tolerance and differing-block threshold.

The dedicated iPad `gles` check requires a qualified `home` reference and FAILs
explicitly when one is absent or invalid. This is missing visual coverage,
not proof of a rendering defect. Boot/counter-only checks remain separately
useful and now state when no frame-reference comparison occurred.

The qualified `k48ap/7B500/home.png` reference comes from a separate fresh
software-CoreAnimation preparation, with live firmware identity and source
hashes in its manifest. Full lock/home captures are retained in the recorded
artifact directory; the clock-varying lock is not committed as a static golden.
Other builds still require independently qualified references. The iPod's
existing references are unchanged.
