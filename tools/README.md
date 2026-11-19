# Tools

| Script | Purpose |
|--------|---------|
| `gen_www.py` | Embed `main/www/index.html` → `main/www_html.h` |
| `bump_version.py` | Sync `VERSION` ↔ `main/version.h` (+ CHANGELOG stub) |
| `build_release.sh` | fullclean + build single `ek410` → `dist/ek410.bin` |
| `flash_release.sh` | Flash bootloader + table + image to both OTA slots |
| `ct_prov_xor_test.py` | SoftAP ↔ BLE in-process XOR smoke over serial |
| `ct_prov_softap_test.py` | SoftAP CT-Prov smoke helper |
| `release_smoke.py` | Release suite: identity + CT-Prov + XOR + MQTT-busy / config consistency |
| `qa_gate.py` | Unified QA gate (`smoke` / `candidate` / `soak`) + JSON report |
| `qa_lib/` | Result model, firmware SHA, power-meter stub |
| `frame_convert.py` | Arbitrary image → 960000-byte 4bpp panel frame |
| `gen_font.py` | 8×16 ASCII font header |
| `gen_font_hz.py` | 16×16 CJK subset font |
| `gen_splash_logo.py` | Brand PNG → `main/splash_logo_data.h` |
| `stability_stress.py` | Longevity / stress helper over serial or HTTP |
| `gpio_scan/` | Standalone IDF project used during pin bring-up |

Deprecated aliases (still call the scripts above): `build_dual_sku.sh`,
`flash_dual_sku.sh`, `ct_prov_dual_test.py`.

## Examples

```bash
python3 tools/gen_www.py
python3 tools/bump_version.py --check

# Single release image (needs IDF env)
bash tools/build_release.sh
bash tools/flash_release.sh /dev/cu.wchusbserialXXXX

python3 tools/ct_prov_xor_test.py --port /dev/cu.wchusbserialXXXX
python3 tools/release_smoke.py --port /dev/cu.wchusbserialXXXX

# Unified gate (same ek410.bin SHA in report)
python3 tools/qa_gate.py --tier smoke --port /dev/cu.wchusbserialXXXX --host 192.168.x.x \
  --json-out /tmp/ek410_qa_smoke.json
python3 tools/qa_gate.py --tier candidate --port PORT --host IP --xor-rounds 5 --stress-rounds 4
# Power probe (optional): EK410_POWER_BACKEND=file EK410_POWER_SOURCE=/tmp/ek410_power.json

python3 tools/gen_splash_logo.py assets/brand/CamThink_LOGO.png -o main/splash_logo_data.h
python3 tools/frame_convert.py input.png output.raw
```

QA plan: [docs/qa_automation.md](../docs/qa_automation.md) · [docs/zh/qa_automation.md](../docs/zh/qa_automation.md).  
CT-Prov details: [docs/ct_prov.md](../docs/ct_prov.md).

`gpio_scan/build/` and `dist/` are gitignored; rebuild artifacts locally.
