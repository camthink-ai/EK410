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

The device no longer returns its MQTT password over HTTP (`getMqttParam` reports `hasPassword` only). The cases that change the broker or publish downlink need an explicit `--mqtt-password`; without it they are skipped, not failed. The business-rhythm soak needs no typed password: `tools/broker_secret.py` takes the credentials a unit already holds out of its NVS into a 0600 file, and `soak_long.py --broker-secret` uses them for a second client on the broker.

## Fix verification (candidate tier)

`candidate` also runs `tools/fix_verification.py`: each fix of a release is checked on the device over HTTP and fails on the previous firmware — nested-JSON rejection, `devName` limit, MQTT password hidden, a failed URL OTA reported through `otaState` **with MQTT back afterwards**, and a local-upload OTA round trip with the image under test. Add the URL display soak with `--verify-url` (an image URL the device can fetch; it also checks that MQTT is back after each display and that PSRAM does not shrink from display to display) and the raw-frame check with `--verify-raw-url` (a raw 960000-byte frame, displayed between ordinary images — the best-quality path needs one ~940 KB PSRAM block); `--power` additionally exercises the `sleepMin` clamp; `fix_verification.py --ota-url https://HOST/ek410.bin` makes the device fetch that image itself — URL OTA is https-only on the device (the check also sees `http://` refused), so the image has to be on a host whose certificate the CA bundle trusts —, reboot into the other slot and confirm it. `GET /api/v1/system/getDiag` carries `heap` and `stackMin` (smallest free stack per task, bytes) — read it before and after every soak. On an ESP32-C61 with Wi-Fi, the web server and MQTT up, `heap.freeInt` should stay above ~15 KB and `largestInt` above 8 KB (MQTT cannot start below that). `--ble-exit` — which `candidate` passes — opens BLE provisioning for 20 s with `openBleProv` and requires that the unit is an MQTT client again afterwards, with at least 2 KB more than MQTT needs in its largest internal block (1.3.10 came out of it with none).

```bash
python3 tools/qa_gate.py --tier candidate --port PORT --host IP --mqtt-password '…' --verify-url http://PC:8000/t.png --verify-raw-url http://PC:8000/frame.bin
python3 tools/fix_verification.py --host IP --url http://PC:8000/t.png --rounds 20 --raw-url http://PC:8000/frame.bin --firmware dist/ek410.bin
```

### Reused tools

`release_smoke.py`, `ct_prov_xor_test.py`, `stability_stress.py`, plus `tools/qa_lib/` (SHA, result model, power stub).

## Business-rhythm soak (72 h)

`tools/soak_long.py` is the unattended counterpart of the candidate tier: one unit, its ordinary traffic, for hours. It serves a generated image set from the host — JPEG landscapes (also as portrait, a banner and a 620 KB photo), a grayscale JPEG, truecolour / six-colour / gray / transparent PNG, a file exactly on the 700 KB limit, a raw frame, redirects, chunked and length-less transfers — plus what a server can do wrong: an HTTP 404, a transfer cut in half, a stall, and a self-signed certificate on an HTTPS server of its own, each of which must fail with the reason the firmware logs. Between pictures come a dashboard canvas, a long text screen, a raw frame through `uploadFrame`, files the firmware must refuse, the web console, optionally a public HTTPS image (the only TLS path that succeeds) and, every `--ota-every-hours`, the same firmware uploaded again. On picture rounds other requests are made while the panel refreshes — status polling and the console page, a `proxyImage`, a second `displayUrl`, and an `uploadFrame`, which must be refused at once — and after every round the saved frame is read back (`GET /api/v1/display/lastFrame`): uploads and raw frames byte-identical to what was sent, canvas and text as the first time, every display having saved a frame of its own. `getDiag` is sampled and MQTT must be back after each round. With `--broker-secret` (`tools/broker_secret.py`) a second client on the unit's broker checks that a truthful telemetry follows every round (frame CRC, preview size), sends every `--mqtt-every` rounds the command over the broker instead of HTTP (`queued`, then `ok` or `error` with the reason) and has every `--ping-every` rounds a ping answered. If somebody opens BLE provisioning or the hotspot on the unit while it runs, that round is not judged; what is: the unit is a client again afterwards.

```bash
python3 tools/soak_long.py --host IP --out ~/ek410-soak --port PORT --hours 72 --image dist/ek410.bin \
    --https-url https://HOST/photo.jpg --https-every 37 --ota-every-hours 24 --broker-secret ~/.ek410-broker.json
python3 tools/soak_long.py --host IP --rounds 2 --no-caffeinate     # smoke test of the tool itself
```

A run is `ok` only with no unexpected reboot, every display giving its intended result, no refused request, MQTT back after every display and never down for a whole gap, the device always reachable, no misbehaviour under concurrent requests, every checked frame as sent and never blank, every planned OTA completed, no fault or defect marker on serial — and with `--broker-secret`: the broker told how each command ended, a truthful telemetry after every round, pings answered, and the unit a client again after any provisioning on it. The exit code is 0 only then. The host must stay awake (on macOS the script re-executes under `caffeinate -i -s`; a closed lid still sleeps a laptop — rounds it slept through are reported, not judged) and nothing else may open the serial port or flash the unit. `touch <out>/STOP` ends a run. `tools/tests/test_soak_dryrun.py` (in `run_host_tests.py`) runs the whole tool against a simulated unit and broker and checks that each kind of fault it can inject is found.
