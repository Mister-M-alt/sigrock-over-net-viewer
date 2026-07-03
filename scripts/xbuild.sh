#!/usr/bin/env bash
# Cross-compile sond for the Pi (aarch64) from this x86-64 host.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export PI_SYSROOT="${PI_SYSROOT:-$ROOT/.pi-sysroot}"

if [[ ! -f "$PI_SYSROOT/usr/lib/aarch64-linux-gnu/pkgconfig/libsigrok.pc" ]]; then
  echo "error: sysroot incomplete at $PI_SYSROOT (run scripts/make-sysroot.sh)" >&2
  exit 1
fi

cmake -S "$ROOT" -B "$ROOT/build-pi" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/aarch64-pi-toolchain.cmake" \
  -DBUILD_SERVER=ON -DBUILD_CLIENT=OFF \
  -DCMAKE_BUILD_TYPE=Release
cmake --build "$ROOT/build-pi"
echo "built: $ROOT/build-pi/server/sond"
file "$ROOT/build-pi/server/sond"
