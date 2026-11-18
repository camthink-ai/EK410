# Development

Companion: [开发说明（中文）](zh/development.md)

## Requirements

- ESP-IDF **5.5.x** (see `main/idf_component.yml`: `>=5.3.0`)
- Target: `esp32c61`
- Python 3.11 recommended for the IDF virtualenv

## Build & flash

```bash
. $HOME/esp/esp-idf-5.5/export.sh
cd /path/to/EK410

idf.py set-target esp32c61          # first time only
idf.py build
idf.py -p /dev/cu.wchusbserialXXXX flash monitor
```

### Dual SKU (SoftAP + BLE CT-Prov)

```bash
bash tools/build_dual_sku.sh
bash tools/flash_dual_sku.sh /dev/cu.wchusbserialXXXX
```

`sdkconfig` is gitignored; SoftAP defaults in `sdkconfig.defaults`, BLE overlay in `sdkconfig.ble_prov`.

## SoftAP Web UI

Edit `main/www/index.html`, then embed:

```bash
python3 tools/gen_www.py
```

## Version management

| File | Role |
|------|------|
| `VERSION` | Canonical SemVer `X.Y.Z` |
| `main/version.h` | C macros for firmware |
| `CHANGELOG.md` | Human-readable history |
| `tools/bump_version.py` | Sync VERSION ↔ version.h (+ CHANGELOG stub) |

```bash
python3 tools/bump_version.py --show
python3 tools/bump_version.py --check
python3 tools/bump_version.py 1.3.0
# edit CHANGELOG.md, commit, then:
git tag -a v1.3.0 -m "EK410 v1.3.0"
git push origin main --tags
```

Rules:

- **MAJOR** — breaking MQTT/HTTP contracts or panel protocol
- **MINOR** — new features, backward compatible
- **PATCH** — fixes / polish
- Git tags: `v` + SemVer (`v1.3.0`)
- Do not change `deviceId` generation lightly — it is the NeoMind identity

## Tools

See [tools/README.md](../tools/README.md).
