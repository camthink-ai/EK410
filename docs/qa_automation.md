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

### Reused tools

`release_smoke.py`, `ct_prov_xor_test.py`, `stability_stress.py`, plus `tools/qa_lib/` (SHA, result model, power stub).
