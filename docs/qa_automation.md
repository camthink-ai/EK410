# EK410 single-firmware QA automation

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

`candidate` also runs `tools/fix_verification.py`: each fix of a release is checked on the device over HTTP and fails on the previous firmware — nested-JSON rejection, `devName` limit, MQTT password hidden, a failed URL OTA reported through `otaState`, and a local-upload OTA round trip with the image under test. Add the URL display soak with `--verify-url` (an image URL the device can fetch); `--power` additionally exercises the `sleepMin` clamp. `getDevInfo` carries `heap` and `stackMin` (smallest free stack per task, bytes) — read them before and after every soak.

```bash
python3 tools/qa_gate.py --tier candidate --port PORT --host IP --mqtt-password '…' --verify-url http://PC:8000/t.png
python3 tools/fix_verification.py --host IP --url http://PC:8000/t.png --rounds 20 --firmware dist/ek410.bin
```

### Reused tools

`release_smoke.py`, `ct_prov_xor_test.py`, `stability_stress.py`, plus `tools/qa_lib/` (SHA, result model, power stub).
