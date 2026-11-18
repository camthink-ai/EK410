#!/usr/bin/env bash
# Legacy name: builds the single EK410 image (SoftAP XOR BLE in one firmware).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

if [[ -z "${IDF_PATH:-}" ]]; then
  echo "IDF_PATH not set — source export.sh first" >&2
  exit 1
fi

DIST="$ROOT/dist"
mkdir -p "$DIST"

echo "==> Single image ek410 (SoftAP XOR BLE CT-Prov)"
idf.py fullclean || true
idf.py set-target esp32c61
idf.py build
cp -f build/ek410.bin "$DIST/ek410.bin"
cp -f build/bootloader/bootloader.bin "$DIST/bootloader.bin"
cp -f build/partition_table/partition-table.bin "$DIST/partition-table.bin"
cp -f build/ota_data_initial.bin "$DIST/ota_data_initial.bin"

python3 - <<'PY'
from pathlib import Path
import struct
p = Path("dist/ek410.bin")
data = p.read_bytes()
# app descriptor magic at offset 0x20 in ESP app image segment — use esp_app_desc via string scan
name = b"ek410\0"
assert name in data, "project_name ek410 not found in binary"
print(f"OK {p} ({p.stat().st_size} bytes)")
PY

echo "Done — flash with: bash tools/flash_dual_sku.sh PORT"
