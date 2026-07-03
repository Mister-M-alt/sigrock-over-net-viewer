#!/usr/bin/env bash
# Copy the cross-built sond to the Pi and run it (defaults to a full scan).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PI="${PI:-amx-pi}"
BIN="$ROOT/build-pi/server/sond"

[[ -f "$BIN" ]] || { echo "error: $BIN not built (run scripts/xbuild.sh)" >&2; exit 1; }
scp "$BIN" "$PI:/tmp/sond"
echo "--- running /tmp/sond ${*:-} on $PI ---"
ssh "$PI" "/tmp/sond ${*:-}"
