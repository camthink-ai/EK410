# Tools

| Script | Purpose |
|--------|---------|
| `gen_www.py` | Embed `main/www/index.html` → `main/www_html.h` |
| `bump_version.py` | Sync `VERSION` ↔ `main/version.h` (+ CHANGELOG stub) |
| `frame_convert.py` | Arbitrary image → 960000-byte 4bpp panel frame |
| `gen_font.py` | 8×16 ASCII font header |
| `gen_font_hz.py` | 16×16 CJK subset font |
| `gen_splash_logo.py` | Brand PNG → `main/splash_logo_data.h` |
| `stability_stress.py` | Longevity / stress helper over serial or HTTP |
| `gpio_scan/` | Standalone IDF project used during pin bring-up |

## Examples

```bash
python3 tools/gen_www.py
python3 tools/bump_version.py --check
python3 tools/gen_splash_logo.py assets/brand/CamThink_LOGO.png -o main/splash_logo_data.h
python3 tools/frame_convert.py input.png output.raw
```

`gpio_scan/build/` is gitignored; build only when diagnosing hardware.
