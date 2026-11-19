#!/usr/bin/env bash
# Deprecated alias — use tools/flash_release.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
echo "NOTE: flash_dual_sku.sh is deprecated; calling flash_release.sh" >&2
exec bash "$ROOT/tools/flash_release.sh" "$@"
