#!/usr/bin/env python3
"""Convert an image into the raw 4bpp frame consumed by `display_url`.

Panel: 1200 x 1600 total (two driver ICs, 1200x800 each, stacked vertically).
Frame layout = CS0 half (top 800 lines) followed by CS1 half (bottom 800 lines).
Each half: 800 lines x 600 bytes, 2 pixels per byte (high nibble = left pixel).

Color codes: BLACK=0x0 WHITE=0x1 YELLOW=0x2 RED=0x3 BLUE=0x5 GREEN=0x6.
Colors are matched to the nearest of these six; uncodeable pixels fall back to
white/black by luminance.

Usage: python3 frame_convert.py input.png [-o frame.bin]
Then serve it:  python3 -m http.server 8000  (in the same directory)
"""
import argparse
from PIL import Image

PANEL_W, PANEL_H = 1200, 1600
HALF_H = PANEL_H // 2  # 800 lines per driver IC

PALETTE = {
    "black": (0x0, (0, 0, 0)),
    "white": (0x1, (255, 255, 255)),
    "yellow": (0x2, (255, 255, 0)),
    "red": (0x3, (255, 0, 0)),
    "blue": (0x5, (0, 0, 255)),
    "green": (0x6, (0, 255, 0)),
}


def nearest_code(rgb):
    best, bestd = 0x1, 1 << 30
    for code, (r, g, b) in PALETTE.values():
        d = (rgb[0] - r) ** 2 + (rgb[1] - g) ** 2 + (rgb[2] - b) ** 2
        if d < bestd:
            best, bestd = code, d
    return best


def convert(src, dst):
    img = Image.open(src).convert("RGB")
    if img.size != (PANEL_W, PANEL_H):
        print(f"resizing {img.size} -> {(PANEL_W, PANEL_H)}")
        img = img.resize((PANEL_W, PANEL_H))
    px = img.load()

    out = bytearray()
    for half in range(2):
        for y in range(HALF_H):
            gy = half * HALF_H + y
            row = bytearray(PANEL_W // 2)
            for bx in range(len(row)):
                hi = nearest_code(px[bx * 2, gy])
                lo = nearest_code(px[bx * 2 + 1, gy])
                row[bx] = (hi << 4) | lo
            out += row
    with open(dst, "wb") as f:
        f.write(out)
    print(f"wrote {dst}: {len(out)} bytes (expect {PANEL_W * PANEL_H // 2})")


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("input")
    ap.add_argument("-o", default="frame.bin")
    a = ap.parse_args()
    convert(a.input, a.o)
