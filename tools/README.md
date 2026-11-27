# Tools

Repository: [https://github.com/camthink-ai/EK410](https://github.com/camthink-ai/EK410)

| Script | Purpose |
|--------|---------|
| `gen_www.py` | Embed `main/www/index.html` → `main/www_html.h` |
| `bump_version.py` | Sync `VERSION` ↔ `main/version.h` (+ CHANGELOG stub) |
| `build_release.sh` | fullclean (fatal when it fails) + a config regenerated from `sdkconfig.defaults` + build single `ek410` → `dist/ek410.bin` |
| `flash_release.sh` | Flash bootloader + table + image to both OTA slots |
| `ct_prov_xor_test.py` | SoftAP ↔ BLE in-process XOR smoke over serial |
| `ct_prov_softap_test.py` | SoftAP CT-Prov smoke helper |
| `release_smoke.py` | Release suite: identity + CT-Prov + XOR + MQTT-busy / config consistency. `--mqtt-password` for the broker-change step (skipped without it) |
| `qa_gate.py` | Unified QA gate (`smoke` / `candidate` / `soak`) + JSON report. Binds the device `fwVersion` to the image, an all-skipped run is not `ok`, forwards `--mqtt-password`; `candidate` also runs `fix_verification.py` (`--verify-url`, `--verify-rounds`, `--verify-raw-url`, `--ble-exit`) |
| `fix_verification.py` | Checks every 1.3.10 and 1.3.11 fix on a real device over HTTP (fails on the firmware that had the bug): JSON depth rejected, `devName` limit, MQTT password hidden, internal-heap headroom (`getDiag`), failed URL OTA reported with MQTT back afterwards, `--url` display soak with MQTT back after each display and no PSRAM loss between displays, `--raw-url` raw frame displayed between ordinary images, `--firmware` local OTA round trip, `--ota-url` URL OTA round trip with an `https://` image URL (the device fetches it itself; `http://` is refused), `--power` `sleepMin` clamp, `--ble-exit` the unit is an MQTT client again after leaving BLE provisioning, with room for the next client start — also after one more picture when `--url` is given (1.3.10 came out of it with none; until the last 1.3.11 build the provisioning worker task stayed behind and cut the largest block to 8704) |
| `soak_long.py` | Business-rhythm soak of one unit (72 h by default), seen from both its sides — HTTP, and with `--broker-secret` the broker it publishes to. Serves a generated image set from this computer: smooth landscapes as JPEG (landscape, portrait, a banner, a 620 KB photo), a grayscale JPEG, PNG (truecolour, six-colour, gray, with transparency), a file exactly on the 700 KB limit, a raw frame, redirects, chunked and length-less transfers — and what a server can do wrong: HTTP 404, a transfer cut in half, a stall, a self-signed certificate on its own HTTPS server, each of which must fail with the reason the firmware logs. Between pictures: a dashboard canvas, a long text screen, an `uploadFrame`, files the firmware must refuse, the web console, optionally a public HTTPS image and — every `--ota-every-hours` — the same firmware uploaded again (the unit must come back in the other slot). While a picture refreshes, the other requests a console user makes: status polling and the page, a `proxyImage`, a second `displayUrl` (queued), and an `uploadFrame`, which has to be refused at once with `panel busy` (until 1.3.10 it waited for the panel with its body unread, and the unit lost Wi-Fi every time). After every round the saved frame is read back: an upload or a raw frame byte-identical to what was sent, canvas and text as the first time, never blank, and every display must have saved a frame; `getDiag` is sampled and MQTT must be back. With `--broker-secret` (see `broker_secret.py`) a second client on the broker checks that a truthful telemetry follows every round (frame CRC, preview size), every `--mqtt-every` rounds the command goes over the broker instead (`queued`, then `ok` / `error` with the reason), and every `--ping-every` rounds a ping is answered. BLE provisioning or the hotspot opened on the unit while it runs (a button, the app) is not a failed round: the run waits and checks that the unit is a client again afterwards. Appends `rounds.jsonl`, rewrites `summary.json`, keeps the serial log with `--port` and fails on fault and defect markers in it; never touches the device's configuration. `<out>/STOP` ends a run. `tests/test_soak_dryrun.py` runs the whole tool against a simulated unit and broker |
| `broker_secret.py` | Takes the broker credentials a unit already holds out of its NVS over serial (the API reports `hasPassword` only) into a mode-0600 file outside the repository, for `soak_long.py --broker-secret`. Prints key names and lengths only; resets the unit, writes nothing |
| `run_host_tests.py` | Host regression tests, no device / no ESP-IDF (~10 s): JSON depth guard, the panel-frame dither (`main/img_dither.c` compiled on the host, plus a byte-for-byte frame layout match with `frame_convert.py`), native frame layout, QA gate semantics, web page, fonts, repo hygiene, the dither with the EXIF orientation (all eight give the upright frame, byte for byte), and a dry run of `soak_long.py` against a simulated unit and broker (every kind of round; each injected fault is found); `build_release.sh` runs them |
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
# --raw-url: a raw 960000-byte native frame the device can fetch, displayed between ordinary images
python3 tools/fix_verification.py --host IP --url http://PC:8000/t.png --raw-url http://PC:8000/frame.bin
python3 tools/qa_gate.py --tier candidate --port PORT --host IP --verify-url http://PC:8000/t.png --verify-raw-url http://PC:8000/frame.bin
```

QA plan: [docs/qa_automation.md](../docs/qa_automation.md) · [docs/zh/qa_automation.md](../docs/zh/qa_automation.md).  
CT-Prov details: [docs/ct_prov.md](../docs/ct_prov.md).

`gpio_scan/build/` and `dist/` are gitignored; rebuild artifacts locally.
