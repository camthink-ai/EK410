# EK410

**CamThink** 7.09″ Spectra 6 e-ink display firmware (ESP32-C61).

[中文文档](README.zh-CN.md) · [Changelog](CHANGELOG.md) · [Docs](docs/)

| | |
|---|---|
| **Product** | EK410 · CamThink E-Ink Display |
| **SoC** | ESP32-C61HR2 (16MB flash, 2MB PSRAM) |
| **Panel** | 7.09″ E Ink Spectra 6 · 1600×1200 · 6 colors |
| **Firmware** | See [`VERSION`](VERSION) · currently **1.2.2** |
| **Platform** | [NeoMind](https://wiki.camthink.ai) · device type `ek410_Eink` |

---

## Features

- **Provisioning** — hold **MAIN** ~2.5s → SoftAP `EK410-{deviceId}` / password `12345678` → `http://192.168.4.1`
- **Web console** — Wi‑Fi, MQTT, image / canvas / Markdown, power, OTA (embedded SPA)
- **Display** — text, canvas (text/rect/QR), full-frame upload, HTTPS URL fetch
- **NeoMind MQTT** — `device/ek410_Eink/{deviceId}/uplink|downlink`
- **OTA** — dual 4MB slots with bootloader rollback
- **Power** — always-on / timed refresh / hibernate (MAIN wake)

### SoftAP image flow

1. Upload a file **or** enter a URL → device `proxyImage` (STA) → **content preview**
2. Adjust fit / Spectra-6 preview → **Push to screen** → client dither → `uploadFrame`

Device-side `displayUrl` remains for MQTT commands and wake URL.

---

## Quick start

```bash
export IDF_PATH=~/esp/esp-idf-5.5
export IDF_PYTHON_ENV_PATH=~/.espressif/python_env/idf5.5_py3.11_env
export PATH="$HOME/.espressif/tools/riscv32-esp-elf/esp-14.2.0_20260121/riscv32-esp-elf/bin:$IDF_PYTHON_ENV_PATH/bin:$PATH"

python3 "$IDF_PATH/tools/idf.py" build
python3 "$IDF_PATH/tools/idf.py" -p PORT flash monitor
```

More detail: [docs/development.md](docs/development.md).

---

## Repository layout

```
EK410/
├── VERSION                 # SemVer source of truth
├── CHANGELOG.md
├── README.md / README.zh-CN.md
├── CMakeLists.txt
├── partitions.csv          # NVS + OTA dual app
├── sdkconfig.defaults
├── dependencies.lock
├── main/                   # Firmware sources
│   ├── version.h           # Generated/synced from VERSION
│   ├── www/                # SoftAP SPA (→ www_html.h via tools/gen_www.py)
│   └── …                   # wifi, mqtt, http, app_epd, epd709, …
├── tools/                  # Build helpers & diagnostics
├── assets/                 # Brand & splash source art
│   ├── brand/
│   └── splash/
└── docs/                   # Hardware, API, development, design preview
    ├── hardware.md
    ├── api.md
    ├── development.md
    ├── zh/                 # Chinese counterparts
    └── design/ui-preview/  # Static UI mock (not flashed)
```

---

## Versioning

Semantic Versioning (`MAJOR.MINOR.PATCH`).

| Artifact | Purpose |
|----------|---------|
| [`VERSION`](VERSION) | Canonical version |
| [`main/version.h`](main/version.h) | Embedded in firmware (`getDevInfo` / MQTT) |
| [`CHANGELOG.md`](CHANGELOG.md) | Release notes (EN + 中文) |
| Git tag | `v1.2.1` |

```bash
python3 tools/bump_version.py --check
python3 tools/bump_version.py 1.3.0   # updates VERSION + version.h + CHANGELOG stub
git tag -a v1.3.0 -m "EK410 v1.3.0"
```

Full process: [docs/development.md](docs/development.md#version-management).

---

## Documentation

| Doc | EN | 中文 |
|-----|----|------|
| Hardware | [docs/hardware.md](docs/hardware.md) | [docs/zh/hardware.md](docs/zh/hardware.md) |
| HTTP API | [docs/api.md](docs/api.md) | [docs/zh/api.md](docs/zh/api.md) |
| Development | [docs/development.md](docs/development.md) | [docs/zh/development.md](docs/zh/development.md) |
| Tools | [tools/README.md](tools/README.md) | — |
| UI design preview | [docs/design/ui-preview/](docs/design/ui-preview/) | [DESIGN.md](docs/design/ui-preview/DESIGN.md) |

---

## NeoMind

After Wi‑Fi + MQTT are configured, the device appears under NeoMind → Devices (Pending). Approve to attach rules / agents that push content to the panel.

Device types live in the **NeoMind-DeviceTypes** repository (`ek410_Eink`).

---

## License

Copyright © CamThink. All rights reserved unless otherwise stated in a LICENSE file.
