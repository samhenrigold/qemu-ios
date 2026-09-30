#!/bin/bash
# Usage: fresh-device.sh IPSW OUT [firmwarekit create options]
# ENTRY selects a shared catalog entry (default n72ap-7E18).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
exec "$ROOT/tests/fresh-device.sh" "${ENTRY:-n72ap-7E18}" "$@"
