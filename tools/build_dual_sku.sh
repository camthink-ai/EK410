#!/usr/bin/env bash
# Build SoftAP (ek410) + BLE (ek410_ble) images into dist/dual_sku/
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

export IDF_PATH="${IDF_PATH:-$HOME/esp/esp-idf-5.5}"
# Prefer the ESP-IDF 3.11 venv when the login shell defaults to a newer Python.
if [[ -d "$HOME/.espressif/python_env/idf5.5_py3.11_env/bin" ]]; then
  export PATH="$HOME/.espressif/python_env/idf5.5_py3.11_env/bin:/opt/homebrew/opt/python@3.11/bin:${PATH:-}"
fi
# shellcheck disable=SC1091
source "$IDF_PATH/export.sh" >/tmp/ek410_idf_export.log 2>&1
command -v idf.py >/dev/null || { echo "idf.py missing after export — see /tmp/ek410_idf_export.log"; exit 1; }

DIST="$ROOT/dist/dual_sku"
mkdir -p "$DIST"

echo "==> SoftAP SKU (build/)"
unset EK410_BLE_BUILD || true
idf.py -B build reconfigure build
cp -f build/ek410.bin "$DIST/ek410_softap.bin"
cp -f build/bootloader/bootloader.bin "$DIST/bootloader.bin"
cp -f build/partition_table/partition-table.bin "$DIST/partition-table.bin"
cp -f build/ota_data_initial.bin "$DIST/ota_data_initial.bin" 2>/dev/null || true

echo "==> BLE SKU (build_ble/) — EK410_BLE_BUILD sets SDKCONFIG_DEFAULTS in CMakeLists"
rm -rf build_ble
EK410_BLE_BUILD=1 idf.py -B build_ble reconfigure build
cp -f build_ble/ek410_ble.bin "$DIST/ek410_ble.bin"

# Sanity: project_name + NimBLE presence
python3 - <<'PY'
from pathlib import Path
import struct, sys
def desc(path):
    data = Path(path).read_bytes()
    magic = struct.pack("<I", 0xABCD5432)
    idx = data.find(magic)
    assert idx >= 0, path
    name = data[idx+48:idx+80].split(b"\0", 1)[0].decode()
    return name
sa = desc(f"{Path('dist/dual_sku')}/ek410_softap.bin")
bl = desc(f"{Path('dist/dual_sku')}/ek410_ble.bin")
print(f"project_name softap={sa!r} ble={bl!r}")
assert sa == "ek410", sa
assert bl == "ek410_ble", bl
sz_s = Path("dist/dual_sku/ek410_softap.bin").stat().st_size
sz_b = Path("dist/dual_sku/ek410_ble.bin").stat().st_size
print(f"sizes softap={sz_s} ble={sz_b}")
assert sz_b > sz_s, "BLE image should be larger (NimBLE linked)"
print("SKU sanity OK")
PY

ROOT="$ROOT" python3 - <<'PY'
import os, json
root = os.environ["ROOT"]
dist = os.path.join(root, "dist/dual_sku")
meta = {
  "softap": "ek410_softap.bin",
  "ble": "ek410_ble.bin",
  "softap_offset": "0x20000",   # ota_0
  "ble_offset": "0x420000",     # ota_1
  "bootloader_offset": "0x0",
  "partition_offset": "0x8000",
  "otadata_offset": "0x10000",
}
open(os.path.join(dist, "manifest.json"), "w").write(json.dumps(meta, indent=2) + "\n")
print("Wrote", os.path.join(dist, "manifest.json"))
for n in ("ek410_softap.bin", "ek410_ble.bin", "bootloader.bin", "partition-table.bin"):
    p = os.path.join(dist, n)
    print(f"  {n}: {os.path.getsize(p)} bytes")
PY

echo "Done → $DIST"
