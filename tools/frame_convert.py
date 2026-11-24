#!/usr/bin/env python3
"""Convert a landscape 1600x1200 image into the raw native frame that a
`displayUrl` / `wakeUrl` .raw fetch streams straight to the panel.

Native layout (main/epd709.h; stream_raw_native / stream_render_unlocked in
main/app_epd.c):
- Two driver ICs side by side: CS0 drives native x 0..599, CS1 x 600..1199 of a
  1200 x 1600 portrait raster.
- Frame = CS0 half (1600 lines x 300 bytes) followed by the CS1 half,
  960000 bytes in total.
- Each byte packs 2 native pixels, high nibble = left one.
- Logical landscape 1600x1200 is that raster rotated 90 deg CCW:
  lx = ny, ly = 1199 - nx  (inverse: nx = 1199 - ly, ny = lx).

Colour codes (4 bits): BLACK=0x0 WHITE=0x1 YELLOW=0x2 RED=0x3 BLUE=0x5 GREEN=0x6.
Colours are matched to the nearest of these six.

Usage: python3 frame_convert.py input.png -o frame.bin
Then serve it:  python3 -m http.server 8000  (in the same directory)
"""
import argparse
from PIL import Image

PANEL_W, PANEL_H = 1600, 1200  # logical landscape
NATIVE_W, NATIVE_H = 1200, 1600
CS_PX = 600  # pixels per driver IC
CS_BYTES = CS_PX // 2  # 300 bytes per native line
HALF_BYTES = CS_BYTES * NATIVE_H  # 480000 per driver IC

PALETTE = (
    (0x0, (0, 0, 0)),       # black
    (0x1, (255, 255, 255)), # white
    (0x2, (255, 255, 0)),   # yellow
    (0x3, (255, 0, 0)),     # red
    (0x5, (0, 0, 255)),     # blue
    (0x6, (0, 255, 0)),     # green
)


def nearest_code(rgb):
    best, bestd = 0x1, 1 << 30
    for code, (r, g, b) in PALETTE:
        d = (rgb[0] - r) ** 2 + (rgb[1] - g) ** 2 + (rgb[2] - b) ** 2
        if d < bestd:
            best, bestd = code, d
    return best


def pack_native(code):
    """Byte for byte what stream_render_unlocked() sends for a logical image:
    `code(lx, ly)` returns the 4-bit colour of a logical landscape pixel."""
    out = bytearray()
    for csx in range(2):
        for ny in range(NATIVE_H):
            for b in range(CS_BYTES):
                nx = csx * CS_PX + b * 2
                hi = code(ny, NATIVE_W - 1 - nx)
                lo = code(ny, NATIVE_W - 1 - (nx + 1))
                out.append(((hi & 0x0F) << 4) | (lo & 0x0F))
    return out


def convert(src, dst):
    img = Image.open(src).convert("RGB")
    if img.size != (PANEL_W, PANEL_H):
        print(f"resizing {img.size} -> {(PANEL_W, PANEL_H)}")
        img = img.resize((PANEL_W, PANEL_H), Image.LANCZOS)
    px = img.load()
    cache = {}

    def code(lx, ly):
        rgb = px[lx, ly]
        c = cache.get(rgb)
        if c is None:
            c = cache[rgb] = nearest_code(rgb)
        return c

    out = pack_native(code)
    with open(dst, "wb") as f:
        f.write(out)
    print(f"wrote {dst}: {len(out)} bytes (expect {HALF_BYTES * 2})")


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("input")
    ap.add_argument("-o", default="frame.bin")
    a = ap.parse_args()
    convert(a.input, a.o)
