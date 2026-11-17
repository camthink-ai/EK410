#!/usr/bin/env bash
# Flash SoftAP → ota_0, BLE → ota_1, boot SoftAP.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DIST="$ROOT/dist/dual_sku"
PORT="${1:-/dev/cu.wchusbserial3130}"

export IDF_PATH="${IDF_PATH:-$HOME/esp/esp-idf-5.5}"
if [[ -d "$HOME/.espressif/python_env/idf5.5_py3.11_env/bin" ]]; then
  export PATH="$HOME/.espressif/python_env/idf5.5_py3.11_env/bin:/opt/homebrew/opt/python@3.11/bin:${PATH:-}"
fi
# shellcheck disable=SC1091
source "$IDF_PATH/export.sh" >/tmp/ek410_idf_export.log 2>&1
command -v esptool.py >/dev/null || { echo "esptool.py missing after export"; exit 1; }

test -f "$DIST/ek410_softap.bin"
test -f "$DIST/ek410_ble.bin"

echo "Flashing dual SKU to $PORT"
esptool.py --chip esp32c61 -p "$PORT" -b 460800 \
  --before default_reset --after hard_reset write_flash \
  --flash_mode dio --flash_freq 80m --flash_size 16MB \
  0x0       "$DIST/bootloader.bin" \
  0x8000    "$DIST/partition-table.bin" \
  0x10000   "$DIST/ota_data_initial.bin" \
  0x20000   "$DIST/ek410_softap.bin" \
  0x420000  "$DIST/ek410_ble.bin"

echo "Done — device boots SoftAP SKU (ota_0)"
