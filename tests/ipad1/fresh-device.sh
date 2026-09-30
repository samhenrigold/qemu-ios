#!/bin/bash
# Usage: fresh-device.sh IPSW OUT [firmwarekit create options]
# ENTRY selects a shared catalog entry (default k48ap-7B500).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
exec "$ROOT/tests/fresh-device.sh" "${ENTRY:-k48ap-7B500}" "$@"
