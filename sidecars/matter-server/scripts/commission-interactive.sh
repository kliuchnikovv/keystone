#!/usr/bin/env bash
# Interactive Matter commissioning helper.
#
# Boots the sidecar in the background, prompts for the 11-digit setup code
# (which minimises the latency between "Apple Home shows the code" and the
# sidecar starting PASE), runs the commission → On → Off end-to-end, then
# tears the sidecar down cleanly.
#
# Usage:
#   ./scripts/commission-interactive.sh
#
# Requires: dist/ already built (npm run build).

set -euo pipefail

SIDECAR_DIR="$(cd "$(dirname "$0")/.." && pwd)"
LOG_FILE="${MATTER_LOG_FILE:-/tmp/keystone-matter-sidecar.log}"
STORAGE="${KEYSTONE_MATTER_STORAGE:-/tmp/matter-data}"

cleanup() {
    if [[ -n "${SPID:-}" ]] && kill -0 "$SPID" 2>/dev/null; then
        echo "→ stopping sidecar (pid $SPID)…"
        kill "$SPID" 2>/dev/null || true
        wait "$SPID" 2>/dev/null || true
    fi
}
trap cleanup EXIT INT TERM

if lsof -iUDP:5540 >/dev/null 2>&1; then
    echo "⚠️  Something is already listening on UDP:5540. Kill it before continuing:" >&2
    lsof -iUDP:5540 >&2
    exit 1
fi

echo "→ wiping stale storage at $STORAGE"
rm -rf "$STORAGE"

echo "→ starting matter sidecar (log: $LOG_FILE)"
KEYSTONE_MATTER_STORAGE="$STORAGE" node "$SIDECAR_DIR/dist/index.js" > "$LOG_FILE" 2>&1 &
SPID=$!

for _ in $(seq 1 20); do
    if grep -q "ws server listening" "$LOG_FILE" 2>/dev/null; then break; fi
    sleep 0.25
done
if ! grep -q "ws server listening" "$LOG_FILE" 2>/dev/null; then
    echo "❌ sidecar failed to start; log:" >&2
    tail -20 "$LOG_FILE" >&2
    exit 1
fi
echo "✓ sidecar ready"
echo ""
echo "Now, in Apple Home:"
echo "  1) Open WARMBLIXT → gear icon → Turn On Pairing Mode"
echo "  2) Copy the 11-digit setup code"
echo ""
read -r -p "Paste setup code (11 digits, dashes optional): " CODE

CODE_CLEAN=$(echo "$CODE" | tr -cd 0-9)
if [[ ${#CODE_CLEAN} -lt 11 ]]; then
    echo "❌ setup code too short after stripping dashes (got ${#CODE_CLEAN}, need 11+)" >&2
    exit 1
fi

echo "→ running commission → listNodes → On → Off"
SETUP_CODE="$CODE_CLEAN" node "$SIDECAR_DIR/scripts/commission-and-toggle.mjs"
