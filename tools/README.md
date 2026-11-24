# Tools

| Script | Purpose |
|--------|---------|
| `gen_www.py` | Embed `main/www/index.html` → `main/www_html.h` |
| `bump_version.py` | Sync `VERSION` ↔ `main/version.h` (+ CHANGELOG stub) |
| `build_release.sh` | fullclean + build single `ek410` → `dist/ek410.bin` |
| `flash_release.sh` | Flash bootloader + table + image to both OTA slots |
| `ct_prov_xor_test.py` | SoftAP ↔ BLE in-process XOR smoke over serial |
| `ct_prov_softap_test.py` | SoftAP CT-Prov smoke helper |
| `release_smoke.py` | Release suite: identity + CT-Prov + XOR + MQTT-busy / config consistency. `--mqtt-password` for the broker-change step (skipped without it) |
| `qa_gate.py` | Unified QA gate (`smoke` / `candidate` / `soak`) + JSON report. Binds the device `fwVersion` to the image, an all-skipped run is not `ok`, forwards `--mqtt-password`; `candidate` also runs `fix_verification.py` |
| `fix_verification.py` | Checks every 1.3.10 fix on a real device over HTTP (fails on 1.3.9): JSON depth, `devName` limit, MQTT password hidden, failed URL OTA reported, `--url` display soak, `--firmware` local OTA round trip, `--power` `sleepMin` clamp |
| `run_host_tests.py` | Host regression tests, no device / no ESP-IDF (~10 s); `build_release.sh` runs them |
| `tests/` | The tests behind the runner: JSON depth guard (C), native frame layout, QA gate semantics, embedded web page, font tables, repo hygiene |
| `qa_lib/` | Result model, firmware SHA / embedded version, power-meter stub |
| `frame_convert.py` | Landscape 1600×1200 image → 960000-byte native panel frame (CS0/CS1 halves, 90° rotated, 4-bit Spectra 6 codes) |
| `gen_font.py` | 8×16 ASCII font header from Noto Sans Mono CJK SC (OFL) — `EK410_MONO_FONT=<path>`; font not committed |
| `gen_font_hz.py` | 16×16 CJK subset font from Noto Sans CJK SC (OFL) — `EK410_CJK_FONT=<path>`; font not committed |
| `gen_splash_logo.py` | Brand PNG → `main/splash_logo_data.h` |
| `stability_stress.py` | Longevity / stress helper over serial or HTTP (`--mqtt-password` for the broker-downlink case) |
| `gpio_scan/` | Standalone IDF project used during pin bring-up |

Deprecated aliases (still call the scripts above): `build_dual_sku.sh`,
`flash_dual_sku.sh`, `ct_prov_dual_test.py`.

## Examples

```bash
python3 tools/run_host_tests.py          # no device needed; exit 1 on any failure
python3 tools/bump_version.py --check
python3 tools/gen_www.py

# Single release image (needs IDF env)
bash tools/build_release.sh
bash tools/flash_release.sh /dev/cu.wchusbserialXXXX

python3 tools/ct_prov_xor_test.py --port /dev/cu.wchusbserialXXXX
python3 tools/release_smoke.py --port /dev/cu.wchusbserialXXXX

# Unified gate (same ek410.bin SHA in report)
python3 tools/qa_gate.py --tier smoke --port /dev/cu.wchusbserialXXXX --host 192.168.x.x \
  --json-out /tmp/ek410_qa_smoke.json
python3 tools/qa_gate.py --tier candidate --port PORT --host IP --xor-rounds 5 --stress-rounds 4
# The cases that change the broker or publish downlink need the device's MQTT
# password (it is write-only over HTTP now) — they are skipped without it:
python3 tools/qa_gate.py --tier candidate --port PORT --host IP --mqtt-password '…' \
  --json-out /tmp/ek410_qa_candidate.json
# Power probe (optional): EK410_POWER_BACKEND=file EK410_POWER_SOURCE=/tmp/ek410_power.json

python3 tools/gen_splash_logo.py assets/brand/CamThink_LOGO.png -o main/splash_logo_data.h
# Landscape 1600x1200 image -> native frame (what a .raw displayUrl / uploadFrame expects)
python3 tools/frame_convert.py input.png -o frame.bin
# Regenerate the CJK font table (Noto Sans CJK SC is not committed):
EK410_CJK_FONT=/path/to/NotoSansCJKsc-Regular.otf python3 tools/gen_font_hz.py
EK410_MONO_FONT=/path/to/NotoSansMonoCJKsc-Regular.otf python3 tools/gen_font.py

# On-device verification of the 1.3.10 fixes (device IP; LAN or 192.168.4.1)
python3 tools/fix_verification.py --host IP
python3 tools/fix_verification.py --host IP --url http://PC:8000/t.png --rounds 20 --firmware dist/ek410.bin
```

QA plan: [docs/qa_automation.md](../docs/qa_automation.md) · [docs/zh/qa_automation.md](../docs/zh/qa_automation.md).  
CT-Prov details: [docs/ct_prov.md](../docs/ct_prov.md).

`gpio_scan/build/` and `dist/` are gitignored; rebuild artifacts locally.
