# EK410

[![Platform](https://img.shields.io/badge/platform-ESP32--C61-blue)]()
[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-5.5-orange)]()
[![Version](https://img.shields.io/badge/version-1.3.0-blue)](VERSION)
[![License](https://img.shields.io/badge/license-Dual--License-red)](LICENSE)

**CamThink** 7.09″ Spectra 6 e-ink display firmware.

[中文文档](README.zh-CN.md) · [Changelog](CHANGELOG.md) · [Docs](docs/) · [License](LICENSE)

| | |
|---|---|
| **Product** | EK410 · CamThink E-Ink Display |
| **SoC** | ESP32-C61HR2 · 16MB flash · 2MB PSRAM |
| **Panel** | E Ink Spectra 6 · logical 1600×1200 · 6 colors |
| **Firmware** | [`VERSION`](VERSION) → **1.3.0** |
| **Platform** | [NeoMind](https://wiki.camthink.ai) · device type `ek410_Eink` |

---

## Overview

EK410 is a battery-friendly e-ink endpoint: SoftAP / BLE provisioning, embedded web console, NeoMind MQTT, and dual OTA images (SoftAP UI vs BLE CT-Prov). The panel keeps the last image when powered down; Wi‑Fi and MQTT reconnect automatically while online.

```
┌────────────────────────────────────────────────────────────┐
│  SoftAP web  ·  NeoLink CT-Prov  ·  NeoMind MQTT / HTTP    │
├─────────────────────────────┬──────────────────────────────┤
│  ota_0  ek410 (SoftAP+httpd)│  ota_1  ek410_ble (NimBLE)   │
│  MAIN hold → SoftAP (blue)  │  PAGE hold → BLE (cyan)      │
├─────────────────────────────┴──────────────────────────────┤
│  Spectra 6 panel · lastfrm flash · AW9523B RGB + power     │
└────────────────────────────────────────────────────────────┘
```

ESP32-C61 cannot run SoftAP httpd and NimBLE in one process — product path is **two OTA slots**. Details: [docs/ct_prov.md](docs/ct_prov.md).

---

## Features

- **SoftAP + web console** — Wi‑Fi / MQTT / image / canvas / Markdown / power / OTA
- **NeoLink CT-Prov** — SoftAP HTTP + BLE GATT (dedicated BLE image)
- **Display** — upload frame, URL fetch, canvas, Markdown; last frame in `lastfrm`
- **NeoMind MQTT** — `device/ek410_Eink/{deviceId}/uplink|downlink`
- **Power** — always / cycle (`sleepMin` + `wakeSec`) / hibernate; deep sleep cuts LED & panel rail
- **Resilience** — Wi‑Fi exponential reconnect; MQTT client auto-reconnect; HW RESET leaves provisioning and restores last frame (or CamThink logo)

### SoftAP push flow

1. Upload **or** URL → device `proxyImage` (STA) → content preview  
2. Fit / Spectra-6 preview → **Push** → browser dither → `uploadFrame`  

MQTT / wake URL still use on-device `displayUrl`.

---

## Buttons & LEDs

| Control | Action |
|---------|--------|
| **MAIN** hold ~2.5s | SoftAP on (`EK410-{id}` / `12345678` → `http://192.168.4.1`) · blue |
| **PAGE** hold ~2.5s | Enter / exit BLE CT-Prov SKU · cyan |
| **MAIN+PAGE** hold ~6s | Factory reset (device id preserved) |
| Hardware **RESET** | Exit SoftAP/BLE handoff · restore last frame |
| Chip **BOOT** (IO9) held at reset | Factory reset |

| LED | Meaning |
|-----|---------|
| Blue blink | SoftAP / Wi‑Fi joining |
| Cyan blink | BLE provisioning |
| Green solid | Running OK (STA up) |
| Off in deep sleep | LED + panel rail powered down |

SoftAP / BLE idle auto-close: **2 minutes** (activity extends).

---

## Power modes

| Mode | Behavior |
|------|----------|
| `always` | Stay awake; `sleepNow` → deep sleep until MAIN |
| `cycle` | After wake, stay online `wakeSec` (30–600, default **120**), then sleep `sleepMin` minutes |
| `hibernate` | Sleep until MAIN (save param may sleep immediately) |

---

## Quick start

**Requirements:** ESP-IDF **5.5.x**, target `esp32c61`.

```bash
. $HOME/esp/esp-idf-5.5/export.sh
cd /path/to/EK410

idf.py set-target esp32c61          # first time only
idf.py build
idf.py -p PORT flash monitor
```

### Dual SKU (SoftAP + BLE)

```bash
bash tools/build_dual_sku.sh
bash tools/flash_dual_sku.sh PORT   # SoftAP → ota_0, BLE → ota_1, boot SoftAP
```

More: [docs/development.md](docs/development.md) · [tools/README.md](tools/README.md).

---

## Repository layout

```
EK410/
├── VERSION · LICENSE · CHANGELOG.md
├── CMakeLists.txt · partitions.csv · sdkconfig.defaults · sdkconfig.ble_prov
├── main/                 # firmware (wifi, mqtt, http, ct_prov, app_epd, www/…)
├── tools/                # gen_www, bump_version, dual-SKU build/flash/tests
├── assets/               # brand & splash sources
└── docs/                 # hardware, API, CT-Prov, development
```

`build/`, `build_ble/`, `dist/`, `sdkconfig`, `managed_components/` are gitignored.

---

## Versioning

| Artifact | Role |
|----------|------|
| [`VERSION`](VERSION) | Canonical SemVer |
| [`main/version.h`](main/version.h) | Embedded string |
| [`CHANGELOG.md`](CHANGELOG.md) | Release notes |
| Git tag | `v1.3.0` |

```bash
python3 tools/bump_version.py --check
python3 tools/bump_version.py 1.3.1
git tag -a v1.3.1 -m "EK410 v1.3.1"
```

---

## Documentation

| Topic | EN | 中文 |
|-------|----|------|
| Hardware | [docs/hardware.md](docs/hardware.md) | [docs/zh/hardware.md](docs/zh/hardware.md) |
| HTTP API | [docs/api.md](docs/api.md) | [docs/zh/api.md](docs/zh/api.md) |
| CT-Prov / NeoLink | [docs/ct_prov.md](docs/ct_prov.md) | — |
| Development | [docs/development.md](docs/development.md) | [docs/zh/development.md](docs/zh/development.md) |
| Tools | [tools/README.md](tools/README.md) | — |

---

## NeoMind

Configure Wi‑Fi + MQTT → device appears under NeoMind → Devices (Pending). Device type: **NeoMind-DeviceTypes** / `ek410_Eink`.

---

## License

**Dual-License** (Community + Commercial), same model as [camthink-ai/ne301](https://github.com/camthink-ai/ne301). See [`LICENSE`](LICENSE).
