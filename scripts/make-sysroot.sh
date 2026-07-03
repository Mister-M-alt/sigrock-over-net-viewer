#!/usr/bin/env bash
# Mirror the Pi's rootfs (headers + aarch64 libs) into a local sysroot used for
# cross-compiling. Safe to re-run; rsync only transfers changes.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PI="${PI:-amx-pi}"
SR="${PI_SYSROOT:-$ROOT/.pi-sysroot}"

mkdir -p "$SR/usr"
rsync -a --no-owner --no-group "$PI:/usr/include"              "$SR/usr/"
rsync -a --no-owner --no-group "$PI:/usr/lib/aarch64-linux-gnu" "$SR/usr/lib/"
rsync -a --no-owner --no-group "$PI:/usr/share/pkgconfig"       "$SR/usr/share/" 2>/dev/null || true
ln -sfn usr/lib "$SR/lib"   # usrmerge: satisfy absolute /lib paths in linker scripts
# The loader lives in the triplet dir; recreate the top-level symlink so the ELF
# interpreter path /lib/ld-linux-aarch64.so.1 resolves inside the sysroot at link time.
ln -sfn aarch64-linux-gnu/ld-linux-aarch64.so.1 "$SR/usr/lib/ld-linux-aarch64.so.1"

echo "sysroot ready: $SR ($(du -sh "$SR" | cut -f1))"
