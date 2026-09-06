# App compatibility ledger

A successful launch is not proof of correct rendering, audio, gameplay or network
services. Reviews are scoped to the tested copy and behavior. The older
[compatibility survey](app-compatibility.md) remains historical static analysis.

| App / copy | Runs | Renders | Audio | Input needs | Network needs | Blocker | Evidence |
| --- | --- | --- | --- | --- | --- | --- | --- |
| QEMU iOS Harness, `com.qemuios.harness`, SHA-256 `1c242983e9a336d824b9188ea87f77d34ce91bc39416941abfa9963dad6dd9a8` | Exact foreground app at 30 s | Initial menu reviewed; graphics test not run here | Not exercised here | Touch; not exercised here | Optional HTTP test | No launch blocker observed | [5 s](app-ledger/harness-5s.png), [20 s](app-ledger/harness-20s.png) |

This row was collected on 2026-09-06 with iOS 3.1.3, the `nand-agent-v4` base and
the emulator at `397e5ba97e`. The run confirmed installation, launch and clean
guest shutdown. Its complete local evidence is in
`/var/folders/tp/360v5_ln3lxg5x66gf0rqc540000gn/T/it-ledger-native-vq5liw8e/evidence`.
The screenshots above are retained in the repository.

## Collecting evidence

```
python3 tests/ipod/regress.py --ledger /path/to/ipas --out /path/to/new-evidence
```

Each IPA gets its own disposable guest and overlay, sequentially. The harness
records the exact SHA-256 and bundle ID, captures screenshots at approximately
5 and 20 seconds after the launch command returns, and verifies the exact
foreground app and a lit screen at 30 seconds. Per-run `results.json`, logs,
overlays and screenshots remain available. The top-level `ledger.json` and
`ledger.md` are updated atomically after each app, so completed rows survive an
interrupted batch. Existing output directories are rejected to preserve prior
reviews. Pass normal harness options such as `--usbmuxd`, `--files-dir`, or
`--audio-hw` as needed.

The collector leaves rendering, audio, input and networking marked unreviewed.
A skipped check, failed infrastructure, changed input file, or incomplete run
cannot receive a successful launch verdict. Record human-reviewed findings here
with their evidence; do not mark an entire title verified from one launch.
Legacy Store verdict integration and broader title coverage remain pending.
