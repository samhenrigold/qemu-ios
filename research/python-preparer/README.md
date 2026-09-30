# Frozen preparation oracle

The former Python orchestrator is retained here only while the final Python
output hashes are captured. `imgtools/device.py create` now invokes the Swift
FirmwareKit CLI with the shared app catalog. It never decrypts or builds NAND.
The board-format Python modules are still test oracles pending golden fixtures;
new preparation behavior belongs in FirmwareKit.

Do not run this archived orchestrator as a device builder. Its fixed temporary
cache and marker protocol are obsolete. The archive will be removed after the
cross-check and corpus gates; research and binary-format inspection stay Python.
