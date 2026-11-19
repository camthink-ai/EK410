#!/usr/bin/env python3
"""Deprecated alias — use tools/ct_prov_xor_test.py (single-image SoftAP↔BLE XOR)."""
from __future__ import annotations

import runpy
import sys
from pathlib import Path

print("NOTE: ct_prov_dual_test.py is deprecated; running ct_prov_xor_test.py", file=sys.stderr)
runpy.run_path(str(Path(__file__).with_name("ct_prov_xor_test.py")), run_name="__main__")
