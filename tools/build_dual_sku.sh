#!/usr/bin/env bash
# Deprecated alias — use tools/build_release.sh
# Kept so older docs / CI still resolve to the single-image build.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
echo "NOTE: build_dual_sku.sh is deprecated; calling build_release.sh" >&2
exec bash "$ROOT/tools/build_release.sh"
