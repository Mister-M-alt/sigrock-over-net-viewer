#!/usr/bin/env bash
# Deploy sonview's custom protocol decoders (e.g. i2s_ex) to the Pi.
# libsigrokdecode auto-discovers ~/.local/share/libsigrokdecode/decoders at
# srd_init(NULL) time, so sond picks them up with no extra search path.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PI="${PI:-amx-pi}"
DEST=".local/share/libsigrokdecode/decoders"
ssh "$PI" "mkdir -p $DEST"
rsync -a --delete "$ROOT/decoders/" "$PI:$DEST/"
echo "deployed decoders to $PI:~/$DEST :"
ssh "$PI" "ls $DEST"
