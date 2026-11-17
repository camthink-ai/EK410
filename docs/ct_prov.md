# CT-Prov / NeoLink compatibility (EK410)

EK410 implements the CamThink **CT-Prov/1** SoftAP HTTP binding used by
[NeoLink](https://github.com/Oreofilling/neolink). On ESP32-C61, NimBLE cannot
share a process with SoftAP httpd — BLE CT-Prov is a **separate OTA image**
(`ek410_ble`, see dual-SKU section below).

## SoftAP discovery (MAIN hold 2.5s — blue blink)

| Item | Value |
|------|--------|
| SSID | `EK410_<MAC6>` (last 3 bytes of SoftAP MAC, uppercase hex) |
| Password | unchanged SoftAP password (`12345678`) |
| Gateway | `192.168.4.1` |
| Endpoint | `http://192.168.4.1/api/v1/ctprov` (also on STA IP when online) |

## BLE (PAGE hold 2.5s — cyan) — dual OTA SKU

Same-process SoftAP+httpd+NimBLE is **not** viable on ESP32-C61 (NimBLE BSS
~27KB; `httpd` needs contiguous internal RAM). Product path is **two OTA
images**:

| Slot | Image | Role |
|------|--------|------|
| `ota_0` @ 0x20000 | `ek410` | SoftAP + httpd (default boot) |
| `ota_1` @ 0x420000 | `ek410_ble` | NimBLE CT-Prov only |

PAGE hold / `openBleProv` on SoftAP SKU → OTA boot → `ek410_ble`. BLE SKU
starts NimBLE **before** `esp_wifi_init` (Wi-Fi driver is lazy) so
`largest_int` stays ~34KB for the controller. Cyan LED while BLE is up.

| Exit path (BLE image) | Behavior |
|----------------------|----------|
| PAGE hold again | NimBLE deinit → boot SoftAP SKU |
| Idle (NVS `bleidle`, default ~2 min; CI uses `idleSec`) | same |
| `session.close` | ack, then same |
| MAIN SoftAP | SoftAP-pending → SoftAP SKU |

```bash
# Build + flash both slots (boots SoftAP)
bash tools/build_dual_sku.sh
bash tools/flash_dual_sku.sh /dev/cu.wchusbserial3130

# SoftAP → BLE NimBLE up → idle exit → SoftAP
python3 tools/ct_prov_dual_test.py --port /dev/cu.wchusbserial3130 --idle-sec 22
```

## Supported SoftAP ops

| Op | Notes |
|----|--------|
| `device.info.get` | model=`EK410`, protocol_version=`1` |
| `communication.capabilities.get` | sections: `wifi` (scannable), `mqtt` |
| `communication.scan` | Wi-Fi scan, cursor pages of 8 |
| `config.set` | SoftAP: try STA join then NVS. BLE SKU: **save NVS only** (join on SoftAP SKU after exit) |
| `config.get` | current wifi ssid + mqtt (password omitted) |
| `session.close` | acks; SoftAP closes after ~600 ms |

```bash
python3 tools/ct_prov_softap_test.py --host 192.168.31.42
```
