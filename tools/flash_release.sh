#!/usr/bin/env bash
# Flash the single EK410 release image to ota_0 and mirror to ota_1.
# Factory / line: same ek410.bin for both slots (upgrade rollback only).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DIST="$ROOT/dist"
PORT="${1:-}"

if [[ -z "$PORT" ]]; then
  echo "Usage: $0 PORT" >&2
  exit 1
fi
if [[ -z "${IDF_PATH:-}" ]]; then
  echo "IDF_PATH not set — source export.sh first" >&2
  exit 1
fi

test -f "$DIST/ek410.bin"
test -f "$DIST/bootloader.bin"
test -f "$DIST/partition-table.bin"
test -f "$DIST/ota_data_initial.bin"

echo "Flashing single image to $PORT (ota_0 + ota_1 mirror)"
# Matches build/flash_args; ota_1 mirror @ 0x420000 (partitions.csv)
esptool.py --chip esp32c61 -p "$PORT" -b 460800 \
  --before default_reset --after hard_reset write_flash \
  --flash_mode dio --flash_size 16MB --flash_freq 80m \
  0x0       "$DIST/bootloader.bin" \
  0x8000    "$DIST/partition-table.bin" \
  0x10000   "$DIST/ota_data_initial.bin" \
  0x20000   "$DIST/ek410.bin" \
  0x420000  "$DIST/ek410.bin"

echo "Done — single firmware ek410.bin (SoftAP XOR BLE)"
