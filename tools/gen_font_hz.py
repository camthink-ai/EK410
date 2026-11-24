#!/usr/bin/env python3
"""Generate main/font_hz16.c + .h — 16x16 CJK glyphs (GB2312 repertoire).

Source font: Noto Sans CJK SC Regular (SIL Open Font License 1.1). The
16 MB font is not committed — point EK410_CJK_FONT at a copy, or put one
in assets/fonts/:
  https://github.com/notofonts/noto-cjk  (Sans/OTF/SimplifiedChinese/)
Verified input: Version 1.004,
  sha256 1652500938055a232cfbfa321de6ebaadfc5635dd9f75e369bc991d14a6512dd

Raster parameters are tuned for Noto Regular. Its ascender is much taller
than that of the macOS fonts used before (18 px vs 14 at size 15), so the
glyph is placed by BASELINE, not by the anchor line — anchored at y=0 the
whole ideograph sank ~3 rows and the bottom of nearly every glyph was cut.
Threshold 160: 128 drops strokes of sparse glyphs such as 线/蓝, 176+ fills
dense ones. Residual clipping is 44 glyphs / 74 px across the 7445.
"""
import hashlib
import os
import sys
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
FONT_NAME = "Noto Sans CJK SC"
NOTO_SHA256 = "1652500938055a232cfbfa321de6ebaadfc5635dd9f75e369bc991d14a6512dd"
FONT_CANDIDATES = [
    os.environ.get("EK410_CJK_FONT", ""),
    str(ROOT / "assets/fonts/NotoSansCJKsc-Regular.otf"),
]
SIZE = 15
BASELINE_ROW = 13
THRESHOLD = 160


def pack16(im: Image.Image) -> bytes:
    px = im.load()
    out = bytearray(32)
    for y in range(16):
        row = 0
        for x in range(16):
            if px[x, y][0] < THRESHOLD:
                row |= 0x8000 >> x
        out[y * 2] = (row >> 8) & 0xFF
        out[y * 2 + 1] = row & 0xFF
    return bytes(out)


def main() -> None:
    path = next((p for p in FONT_CANDIDATES if p and Path(p).is_file()), None)
    if path is None:
        raise SystemExit(f"{FONT_NAME} not found: set EK410_CJK_FONT=<path to "
                         "NotoSansCJKsc-Regular.otf>")
    digest = hashlib.sha256(Path(path).read_bytes()).hexdigest()
    if digest != NOTO_SHA256:
        print(f"warning: {path}\n  sha256 {digest}\n  differs from the verified "
              f"input {NOTO_SHA256} — glyphs may differ from the committed table",
              file=sys.stderr)
    font = ImageFont.truetype(path, SIZE, index=0)
    if font.getname()[0] != FONT_NAME:
        raise SystemExit(f"{path} is {font.getname()[0]}, expected {FONT_NAME}")
    y_offset = BASELINE_ROW - font.getmetrics()[0]

    glyphs = []
    seen = set()
    for qu in range(0xA1, 0xF8):
        for wei in range(0xA1, 0xFF):
            try:
                ch = bytes([qu, wei]).decode("gb2312")
            except Exception:
                continue
            if not ch or ch in seen:
                continue
            u = ord(ch)
            if u < 0x80:
                continue
            seen.add(ch)
            im = Image.new("RGB", (16, 16), (255, 255, 255))
            ImageDraw.Draw(im).text((0, y_offset), ch, font=font, fill=(0, 0, 0))
            glyphs.append((u, pack16(im)))

    glyphs.sort(key=lambda t: t[0])
    (ROOT / "main/font_hz16.h").write_text(
        """#ifndef FONT_HZ16_H
#define FONT_HZ16_H
#include <stdint.h>
#include <stdbool.h>

#define HZ16_W 16
#define HZ16_H 16

#ifdef __cplusplus
extern "C" {
#endif

int hz16_glyph_count(void);
/* Binary-search Unicode; fills 32-byte glyph (16 rows x 2 bytes MSB-left). Returns false if missing. */
bool hz16_lookup(uint16_t unicode, uint8_t out[32]);

#ifdef __cplusplus
}
#endif
#endif
"""
    )
    out = ROOT / "main/font_hz16.c"
    with out.open("w") as f:
        f.write("/* Auto-generated CJK 16x16 — tools/gen_font_hz.py\n"
                " * Source font: Noto Sans CJK SC Regular, SIL OFL 1.1. */\n")
        f.write('#include "font_hz16.h"\n#include <stddef.h>\n\n')
        f.write(f"#define HZ16_N {len(glyphs)}\n")
        f.write("static const uint16_t s_hz_u[HZ16_N] = {\n")
        for i, (u, _) in enumerate(glyphs):
            f.write(f" 0x{u:04X},")
            if (i + 1) % 12 == 0:
                f.write("\n")
        f.write("\n};\n")
        f.write("static const uint8_t s_hz_bits[HZ16_N][32] = {\n")
        for _, bits in glyphs:
            f.write(" {" + ",".join(f"0x{b:02X}" for b in bits) + "},\n")
        f.write("};\n\n")
        f.write(
            "int hz16_glyph_count(void) { return HZ16_N; }\n\n"
            "bool hz16_lookup(uint16_t unicode, uint8_t out[32])\n"
            "{\n"
            "    int lo = 0, hi = HZ16_N - 1;\n"
            "    while (lo <= hi) {\n"
            "        int mid = (lo + hi) >> 1;\n"
            "        uint16_t v = s_hz_u[mid];\n"
            "        if (v == unicode) {\n"
            "            for (int i = 0; i < 32; i++) out[i] = s_hz_bits[mid][i];\n"
            "            return true;\n"
            "        }\n"
            "        if (v < unicode) lo = mid + 1;\n"
            "        else hi = mid - 1;\n"
            "    }\n"
            "    return false;\n"
            "}\n"
        )
    print(f"wrote {out} glyphs={len(glyphs)} size={out.stat().st_size} "
          f"font={font.getname()[0]} {SIZE}px baseline_row={BASELINE_ROW} thr={THRESHOLD}")


if __name__ == "__main__":
    main()
