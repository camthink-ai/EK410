# Tools

| Script | Purpose |
|--------|---------|
| `gen_www.py` | Embed `main/www/index.html` → `main/www_html.h` |
| `bump_version.py` | Sync `VERSION` ↔ `main/version.h` (+ CHANGELOG stub) |
| `build_dual_sku.sh` | fullclean + build single `ek410` → `dist/` |
| `flash_dual_sku.sh` | Flash bootloader + table + image to both OTA slots |
| `ct_prov_dual_test.py` | SoftAP ↔ BLE XOR smoke over serial (legacy name) |
| `ct_prov_softap_test.py` | SoftAP CT-Prov smoke helper |
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

# Single image (needs IDF env)
bash tools/build_dual_sku.sh
bash tools/flash_dual_sku.sh /dev/cu.wchusbserialXXXX

python3 tools/gen_splash_logo.py assets/brand/CamThink_LOGO.png -o main/splash_logo_data.h
python3 tools/frame_convert.py input.png output.raw
```

CT-Prov details: [docs/ct_prov.md](../docs/ct_prov.md).

`gpio_scan/build/` and `dist/` are gitignored; rebuild artifacts locally.
