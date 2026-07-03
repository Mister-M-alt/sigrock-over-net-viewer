#!/usr/bin/env bash
# Regenerate the icon PNGs and the embedded header from assets/icon.svg.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT/assets"
for s in 16 32 48 64 128 256; do
  if command -v rsvg-convert >/dev/null; then
    rsvg-convert -w "$s" -h "$s" icon.svg -o "icon-$s.png"
  else
    magick -background none icon.svg -resize "${s}x${s}" "icon-$s.png"
  fi
done
magick icon-64.png -depth 8 RGBA:/tmp/sonview-icon64.rgba
python3 - "$ROOT/client/src/icon_data.h" <<'PY'
import sys
data = open('/tmp/sonview-icon64.rgba','rb').read()
assert len(data) == 64*64*4
with open(sys.argv[1],'w') as f:
    f.write("// sonview window/taskbar icon, 64x64 RGBA. Generated from assets/icon.svg:\n"
            "//   magick assets/icon-64.png -depth 8 RGBA:- | <this array>\n"
            "// Regenerate with scripts/gen-icon.sh after editing the SVG.\n"
            "#pragma once\n\n"
            "static const unsigned int ICON_W = 64, ICON_H = 64;\n"
            "static const unsigned char ICON_RGBA[64 * 64 * 4] = {\n")
    for i in range(0, len(data), 24):
        f.write("    " + ",".join(str(b) for b in data[i:i+24]) + ",\n")
    f.write("};\n")
PY
echo "regenerated assets/icon-*.png and client/src/icon_data.h"
