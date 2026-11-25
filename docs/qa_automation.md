# EK410 single-firmware QA automation

Source: [github.com/camthink-ai/EK410](https://github.com/camthink-ai/EK410)

**Ship and verify one `ek410.bin` (same SHA) for SoftAP, BLE, MQTT, display, sleep, and OTA.**

Gates:

```bash
python3 tools/qa_gate.py --tier smoke --port PORT --host IP
python3 tools/qa_gate.py --tier candidate --port PORT --host IP
python3 tools/qa_gate.py --tier soak --hours 72 --port PORT --host IP
```

Full acceptance matrix, power criteria, and 72h checklist (Chinese): [zh/qa_automation.md](zh/qa_automation.md).

### Result statuses

| status | meaning |
|--------|---------|
| `pass` | Device met the criterion |
| `fail_device` | Device under test failed |
| `fail_fixture` | Bench / script / serial / host error |
| `skipped` | Intentionally not run |
| `invalid` | Missing measurement (e.g. no power meter) — not a pass |

Reports: `--json-out path.json` (suite summary) and optional `--jsonl-out` (per-case lines).

Two gate rules:

- **The report is bound to the image under test**: case `firmware_binding` compares the device's reported `fwVersion` with the version embedded in the image; a mismatch is `fail_device`, so a report cannot certify a build the device is not running.
- **An all-skipped run is not a pass**: `ok=false` (exit 1) when no case passed, even if nothing failed.

The device no longer returns its MQTT password over HTTP (`getMqttParam` reports `hasPassword` only). The cases that change the broker or publish downlink need an explicit `--mqtt-password`; without it they are skipped, not failed.

## Fix verification (candidate tier)

`candidate` also runs `tools/fix_verification.py`: each fix of a release is checked on the device over HTTP and fails on the previous firmware — nested-JSON rejection, `devName` limit, MQTT password hidden, a failed URL OTA reported through `otaState` **with MQTT back afterwards**, and a local-upload OTA round trip with the image under test. Add the URL display soak with `--verify-url` (an image URL the device can fetch; it also checks that MQTT is back after each display and that PSRAM does not shrink from display to display) and the raw-frame check with `--verify-raw-url` (a raw 960000-byte frame, displayed between ordinary images — the best-quality path needs one ~940 KB PSRAM block); `--power` additionally exercises the `sleepMin` clamp; `fix_verification.py --ota-url https://HOST/ek410.bin` makes the device fetch that image itself — URL OTA is https-only on the device (the check also sees `http://` refused), so the image has to be on a host whose certificate the CA bundle trusts —, reboot into the other slot and confirm it. `GET /api/v1/system/getDiag` carries `heap` and `stackMin` (smallest free stack per task, bytes) — read it before and after every soak. On an ESP32-C61 with Wi-Fi, the web server and MQTT up, `heap.freeInt` should stay above ~15 KB and `largestInt` above 8 KB (MQTT cannot start below that).

```bash
python3 tools/qa_gate.py --tier candidate --port PORT --host IP --mqtt-password '…' --verify-url http://PC:8000/t.png --verify-raw-url http://PC:8000/frame.bin
python3 tools/fix_verification.py --host IP --url http://PC:8000/t.png --rounds 20 --raw-url http://PC:8000/frame.bin --firmware dist/ek410.bin
```

### Reused tools

`release_smoke.py`, `ct_prov_xor_test.py`, `stability_stress.py`, plus `tools/qa_lib/` (SHA, result model, power stub).

## Business-rhythm soak (72 h)

`tools/soak_long.py` is the unattended counterpart of the candidate tier: one unit, its ordinary traffic, for hours. It serves a generated image set from the host (JPEG, flat / six-colour / gray PNG, a raw frame, redirects, a file exactly on the 700 KB limit) and cycles `displayUrl` while MQTT telemetry keeps running; every few rounds a raw frame goes through `uploadFrame`, a file the firmware must refuse is sent, the web console is loaded and — with `--https-url` — a public HTTPS image is displayed, the only TLS path in the run. After every round it reads `getDiag` (heap, stack minima), checks that MQTT is back and appends one line to `rounds.jsonl`; `summary.json` is rewritten each round. With `--port` the serial log is kept, which is what tells an intended reject from any other failure. It never changes the device's configuration.

```bash
python3 tools/soak_long.py --host IP --out ~/ek410-soak --port PORT --hours 72 --image dist/ek410.bin \
    --https-url https://HOST/photo.jpg --https-every 40
python3 tools/soak_long.py --host IP --rounds 2 --no-caffeinate     # smoke test of the tool itself
```

A run is `ok` only with no unexpected reboot, every display giving its intended result, no refused request, MQTT back after every display, the device always reachable and no fault on serial; the exit code is 0 only then. The host must stay awake (on macOS the script re-executes under `caffeinate -i -s`; a closed lid still sleeps a laptop — rounds it slept through are reported, not judged) and nothing else may open the serial port or flash the unit. `touch <out>/STOP` ends a run.
