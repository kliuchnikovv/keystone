#!/usr/bin/env bash
# Run the matter sidecar in the foreground. Storage persists across restarts
# under $KEYSTONE_MATTER_STORAGE (default: ~/.keystone-matter/data), so
# commissioned devices stay in the fabric between runs.
#
# Usage:
#   ./scripts/sidecar-run.sh
#
# Ctrl-C to stop.

set -euo pipefail

SIDECAR_DIR="$(cd "$(dirname "$0")/.." && pwd)"
STORAGE="${KEYSTONE_MATTER_STORAGE:-$HOME/.keystone-matter/data}"

mkdir -p "$STORAGE"

if lsof -iUDP:5540 >/dev/null 2>&1; then
    echo "⚠️  UDP:5540 already in use — another sidecar is running:" >&2
    lsof -iUDP:5540 >&2
    exit 1
fi

echo "→ storage: $STORAGE"
echo "→ starting sidecar (Ctrl-C to stop)"
exec env KEYSTONE_MATTER_STORAGE="$STORAGE" node "$SIDECAR_DIR/dist/index.js"
