# EK410

**CamThink** 7.09″ Spectra 6 e-ink display firmware (ESP32-C61).

[中文文档](README.zh-CN.md) · [Changelog](CHANGELOG.md) · [Docs](docs/) · [License](LICENSE)

| | |
|---|---|
| **Product** | EK410 · CamThink E-Ink Display |
| **SoC** | ESP32-C61HR2 (16MB flash, 2MB PSRAM) |
| **Panel** | 7.09″ E Ink Spectra 6 · 1600×1200 · 6 colors |
| **Firmware** | See [`VERSION`](VERSION) · currently **1.3.0** |
| **Platform** | [NeoMind](https://wiki.camthink.ai) · device type `ek410_Eink` |

---

## Features

- **SoftAP provisioning** — hold **MAIN** ~2.5s → SoftAP `EK410-{deviceId}` / password `12345678` → `http://192.168.4.1`
- **BLE CT-Prov** — hold **PAGE** ~2.5s → cyan; NeoLink BLE on dedicated `ek410_ble` OTA image ([docs/ct_prov.md](docs/ct_prov.md))
- **Web console** — Wi‑Fi, MQTT, image / canvas / Markdown, power, OTA (embedded SPA)
- **Display** — text, canvas (text/rect/QR), full-frame upload, HTTPS URL fetch; last frame persisted in flash
- **NeoMind MQTT** — `device/ek410_Eink/{deviceId}/uplink|downlink`
- **OTA** — dual 4MB slots (SoftAP SKU + BLE SKU) with bootloader rollback
- **Power** — always-on / timed refresh (`sleepMin` + `wakeSec`) / hibernate (MAIN wake)

### SoftAP image flow

1. Upload a file **or** enter a URL → device `proxyImage` (STA) → **content preview**
2. Adjust fit / Spectra-6 preview → **Push to screen** → client dither → `uploadFrame`

Device-side `displayUrl` remains for MQTT commands and wake URL.

### Dual SKU flash

```bash
bash tools/build_dual_sku.sh
bash tools/flash_dual_sku.sh PORT   # SoftAP → ota_0, BLE → ota_1
```

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
├── VERSION / LICENSE / CHANGELOG.md
├── CMakeLists.txt
├── partitions.csv          # NVS + OTA dual app + lastfrm
├── sdkconfig.defaults
├── sdkconfig.ble_prov      # BLE SKU overlay
├── dependencies.lock
├── main/                   # Firmware sources
│   ├── version.h
│   ├── www/                # SoftAP SPA (→ www_html.h via tools/gen_www.py)
│   └── …
├── tools/                  # Build helpers, dual-SKU, CT-Prov tests
├── assets/                 # Brand & splash source art
└── docs/                   # Hardware, API, CT-Prov, development
```

---

## Versioning

Semantic Versioning (`MAJOR.MINOR.PATCH`).

| Artifact | Purpose |
|----------|---------|
| [`VERSION`](VERSION) | Canonical version |
| [`main/version.h`](main/version.h) | Embedded in firmware (`getDevInfo` / MQTT) |
| [`CHANGELOG.md`](CHANGELOG.md) | Release notes (EN + 中文) |
| Git tag | `v1.3.0` |

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
| CT-Prov / NeoLink | [docs/ct_prov.md](docs/ct_prov.md) | — |
| Development | [docs/development.md](docs/development.md) | [docs/zh/development.md](docs/zh/development.md) |
| Tools | [tools/README.md](tools/README.md) | — |
| UI design preview | [docs/design/ui-preview/](docs/design/ui-preview/) | [DESIGN.md](docs/design/ui-preview/DESIGN.md) |

---

## NeoMind

After Wi‑Fi + MQTT are configured, the device appears under NeoMind → Devices (Pending). Approve to attach rules / agents that push content to the panel.

Device types live in the **NeoMind-DeviceTypes** repository (`ek410_Eink`).

---

## License

Dual-license (Community + Commercial), same terms as [camthink-ai/ne301](https://github.com/camthink-ai/ne301). See [`LICENSE`](LICENSE).
