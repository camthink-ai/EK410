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

### Convenience scripts (single image)

```bash
bash tools/build_release.sh
bash tools/flash_release.sh /dev/cu.wchusbserialXXXX
python3 tools/ct_prov_xor_test.py --port /dev/cu.wchusbserialXXXX
```

One `sdkconfig.defaults` → one `ek410.bin`. SoftAP and BLE CT-Prov share that
image and are XOR at runtime. `sdkconfig` is gitignored.

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
python3 tools/bump_version.py X.Y.Z
# edit CHANGELOG.md, commit, then:
git tag -a vX.Y.Z -m "EK410 vX.Y.Z"
git push origin main --tags   # once a remote is configured
```

Rules:

- **MAJOR** — breaking MQTT/HTTP contracts or panel protocol
- **MINOR** — new features, backward compatible
- **PATCH** — fixes / polish
- Git tags: `v` + SemVer (`vX.Y.Z`, e.g. `v1.3.10`)
- Do not change `deviceId` generation lightly — it is the NeoMind identity

## Tools

See [tools/README.md](../tools/README.md).
