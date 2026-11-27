# CT-Prov / NeoLinker compatibility (EK410)

Firmware source: [github.com/camthink-ai/EK410](https://github.com/camthink-ai/EK410).

End-user SoftAP / BLE steps: [CamThink Wiki](https://wiki.camthink.ai/docs/epaper-ek410-series/user-guide/ek410-softap-provisioning) · [中文](https://wiki.camthink.ai/zh-Hans/docs/epaper-ek410-series/user-guide/ek410-softap-provisioning).

EK410 implements the CamThink **CT-Prov/1** SoftAP HTTP binding used by
[NeoLinker](https://github.com/camthink-ai/NeoLinker). SoftAP httpd and NimBLE
share **one firmware image** and are **mutually exclusive** (never both up).

## SoftAP discovery (MAIN hold 2.5s — blue blink; MAIN click to leave)

| Item | Value |
|------|--------|
| SSID | `EK410_<MAC6>` (last 3 bytes of SoftAP MAC, uppercase hex) |
| Password | unchanged SoftAP password (`12345678`) |
| Gateway | `192.168.4.1` |
| Endpoint | `http://192.168.4.1/api/v1/ctprov` (also on STA IP when online) |

## BLE (PAGE hold 2.5s — cyan)

PAGE hold / `openBleProv` stops SoftAP/httpd/mqtt, then starts NimBLE CT-Prov
in-process (cyan LED). MAIN click / SoftAP open / PAGE again / idle /
`session.close` stops NimBLE and restores httpd (and SoftAP if MAIN requested it).

| Exit path | Behavior |
|-----------|----------|
| MAIN click (1.3.11) | leaves whichever provisioning mode is up: NimBLE deinit → restore httpd; SoftAP → closed |
| PAGE hold again | NimBLE deinit → restore httpd |
| Idle (default ~2 min; CI uses `idleSec`) | same |
| `session.close` | ack, then same |
| MAIN SoftAP | stop BLE → SoftAP up |

`ota_0` / `ota_1` are for **firmware upgrade rollback** only (same `ek410` binary).

```bash
bash tools/build_release.sh
bash tools/flash_release.sh /dev/cu.wchusbserial3130
python3 tools/ct_prov_xor_test.py --port /dev/cu.wchusbserial3130
```

## Supported SoftAP ops

| Op | Notes |
|----|--------|
| `device.info.get` | model=`EK410`, protocol_version=`1` |
| `communication.capabilities.get` | sections: `wifi` (scannable), `mqtt` |
| `communication.scan` | Wi-Fi scan, cursor pages of 8 |
| `config.set` | SoftAP: try STA join then NVS. BLE window: save NVS + best-effort join |
| `config.get` | current wifi ssid + mqtt (password omitted) |
| `session.close` | acks; SoftAP closes after ~600 ms |

```bash
python3 tools/ct_prov_softap_test.py --host 192.168.31.42
```
