# Frozen preparation reference

Device creation is owned by Swift FirmwareKit and the app's shared catalog.
All `imgtools/*_device.py` commands delegate to `imgtools/device.py`, which only
translates inputs and invokes the Swift CLI. `tests/fresh-device.sh` invokes the
Swift CLI directly and fails selected missing prerequisites.

These files contain only the remaining bake reference functions, with all
device builders removed. They are not a supported second pipeline. The independent hashes for N72 NOR/NAND metadata and K48 iBoot/NOR/GID
artifacts are checked into FirmwareKit's `LegacyPreparationGoldens.swift`;
those corpus checks no longer execute a Python builder. N45 bake comparisons
still use the archived module until their package-specific expectations are
frozen. Binary-format inspection tools remain under `imgtools`.

The old orchestrator, cache markers, fixed temporary paths and board builders
are deleted. New preparation behavior belongs in FirmwareKit. Source provenance: qemu-ios `0be1499f24`.
