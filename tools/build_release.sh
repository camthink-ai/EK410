#!/usr/bin/env bash
# Build the single EK410 release image (SoftAP XOR BLE CT-Prov in one firmware).
# Output: dist/ek410.bin (+ bootloader, partition table, ota_data).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

if [[ -z "${IDF_PATH:-}" ]]; then
  echo "IDF_PATH not set — source export.sh first" >&2
  exit 1
fi

DIST="$ROOT/dist"
mkdir -p "$DIST"

echo "==> Preflight: version + host tests + embed www"
python3 "$ROOT/tools/bump_version.py" --check
# JSON depth guard, native frame layout, QA gate semantics, embedded page in
# sync with www/index.html, font tables — fails the build if any regressed.
python3 "$ROOT/tools/run_host_tests.py"
python3 "$ROOT/tools/gen_www.py"

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
import hashlib
p = Path("dist/ek410.bin")
data = p.read_bytes()
assert b"ek410\0" in data, "project_name ek410 not found in binary"
sha = hashlib.sha256(data).hexdigest()[:12]
print(f"OK {p} ({p.stat().st_size} bytes) sha256={sha}…")
PY

echo "Done — flash with: bash tools/flash_release.sh PORT"
