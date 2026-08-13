#!/usr/bin/env bash
# Interactive Matter commissioning helper.
#
# Assumes ./scripts/sidecar-run.sh is already running in another terminal —
# fabric state persists across sidecar restarts under
# ~/.keystone-matter/data by default.
#
# Usage:
#   ./scripts/commission-interactive.sh

set -euo pipefail

SIDECAR_DIR="$(cd "$(dirname "$0")/.." && pwd)"

if ! lsof -iUDP:5540 >/dev/null 2>&1; then
    echo "❌ sidecar is not running." >&2
    echo "   Start it first in another terminal:  ./scripts/sidecar-run.sh" >&2
    exit 1
fi

echo "Sidecar is up. In Apple Home:"
echo "  1) Open WARMBLIXT → gear icon → Turn On Pairing Mode"
echo "  2) Copy the 11-digit setup code"
echo ""
read -r -p "Paste setup code (11 digits, dashes optional): " CODE

CODE_CLEAN=$(echo "$CODE" | tr -cd 0-9)
if [[ ${#CODE_CLEAN} -lt 11 ]]; then
    echo "❌ setup code too short (got ${#CODE_CLEAN}, need 11+)" >&2
    exit 1
fi

echo "→ running commission → listNodes → On → Off"
SETUP_CODE="$CODE_CLEAN" node "$SIDECAR_DIR/scripts/commission-and-toggle.mjs"
