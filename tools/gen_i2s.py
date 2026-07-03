#!/usr/bin/env python3
"""Generate a synthetic standard-I2S (Philips) logic capture for decoder testing.
Output: raw binary, 1 byte/sample, bit0=SCK, bit1=WS, bit2=SD (sigrok 'binary' input,
numchannels=3). Left channel (WS=0) = 0x1234, right (WS=1) = 0x5678, 16-bit, MSB first,
1-bit delay after the WS edge (Philips I2S). SD sampled on SCK rising edge."""
import sys

LEFT, RIGHT, BITS, FRAMES = 0x1234, 0x5678, 16, 6
FMT = sys.argv[2] if len(sys.argv) > 2 else "i2s"
out = bytearray()

def bclk(ws, sd):
    b = (ws << 1) | (sd << 2)
    out.append(b | 0)  # SCK low
    out.append(b | 1)  # SCK high (rising edge -> SD sampled here)

def slot(ws, word):
    data = [(word >> (BITS - 1 - i)) & 1 for i in range(BITS)]  # MSB..LSB
    if FMT == "i2s":       bits = [0] + data          # 1-bit delay after WS edge
    elif FMT == "left":    bits = data                # data aligned to WS edge
    elif FMT == "right":   bits = [0, 0, 0, 0] + data # data ends at next WS edge
    else:                  bits = [0] + data
    for b in bits:
        bclk(ws, b)

for _ in range(FRAMES):
    slot(0, LEFT)
    slot(1, RIGHT)

path = sys.argv[1] if len(sys.argv) > 1 else "i2s.bin"
with open(path, "wb") as f:
    f.write(out)
print("wrote %d samples to %s (L=0x%04X R=0x%04X)" % (len(out), path, LEFT, RIGHT))
