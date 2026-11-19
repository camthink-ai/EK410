"""Power / panel-enable measurement stub for the QA bench.

Configure with env:
  EK410_POWER_BACKEND=none|file|http
  EK410_POWER_SOURCE=path-or-url   (CSV/JSON samples, or HTTP JSON API)

Until a real analyzer adapter is wired, deep-sleep and panel-cut cases are
recorded as invalid/skipped rather than false passes.
"""
from __future__ import annotations

import json
import os
import urllib.request
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Optional


@dataclass
class PowerSample:
    label: str
    current_ua: Optional[float] = None
    voltage_v: Optional[float] = None
    panel_busy: Optional[bool] = None
    rail_enable: Optional[bool] = None
    raw: dict[str, Any] | None = None


class PowerMeter:
    """Minimal read interface — backends return None when unavailable."""

    def available(self) -> bool:
        return False

    def read(self, label: str = "spot") -> Optional[PowerSample]:
        return None

    def backend_name(self) -> str:
        return "none"


class FilePowerMeter(PowerMeter):
    """Read last JSON object from a file written by an external logger."""

    def __init__(self, path: Path):
        self.path = path

    def available(self) -> bool:
        return self.path.is_file()

    def backend_name(self) -> str:
        return f"file:{self.path}"

    def read(self, label: str = "spot") -> Optional[PowerSample]:
        if not self.available():
            return None
        try:
            data = json.loads(self.path.read_text(encoding="utf-8"))
            if isinstance(data, list) and data:
                data = data[-1]
            if not isinstance(data, dict):
                return None
            return PowerSample(
                label=label,
                current_ua=_num(data.get("current_ua", data.get("i_ua"))),
                voltage_v=_num(data.get("voltage_v", data.get("v"))),
                panel_busy=_bool(data.get("panel_busy", data.get("busy"))),
                rail_enable=_bool(data.get("rail_enable", data.get("epd_en"))),
                raw=data,
            )
        except Exception:
            return None


class HttpPowerMeter(PowerMeter):
    def __init__(self, url: str, timeout: float = 3.0):
        self.url = url
        self.timeout = timeout

    def available(self) -> bool:
        return bool(self.url)

    def backend_name(self) -> str:
        return f"http:{self.url}"

    def read(self, label: str = "spot") -> Optional[PowerSample]:
        try:
            with urllib.request.urlopen(self.url, timeout=self.timeout) as r:
                data = json.loads(r.read().decode("utf-8"))
            if not isinstance(data, dict):
                return None
            return PowerSample(
                label=label,
                current_ua=_num(data.get("current_ua", data.get("i_ua"))),
                voltage_v=_num(data.get("voltage_v", data.get("v"))),
                panel_busy=_bool(data.get("panel_busy", data.get("busy"))),
                rail_enable=_bool(data.get("rail_enable", data.get("epd_en"))),
                raw=data,
            )
        except Exception:
            return None


def open_power_meter_from_env() -> PowerMeter:
    backend = (os.environ.get("EK410_POWER_BACKEND") or "none").strip().lower()
    source = (os.environ.get("EK410_POWER_SOURCE") or "").strip()
    if backend in ("", "none", "off"):
        return PowerMeter()
    if backend == "file":
        return FilePowerMeter(Path(source or "/tmp/ek410_power.json"))
    if backend == "http":
        return HttpPowerMeter(source)
    return PowerMeter()


def _num(v: Any) -> Optional[float]:
    if v is None:
        return None
    try:
        return float(v)
    except (TypeError, ValueError):
        return None


def _bool(v: Any) -> Optional[bool]:
    if v is None:
        return None
    if isinstance(v, bool):
        return v
    if isinstance(v, (int, float)):
        return bool(v)
    s = str(v).strip().lower()
    if s in ("1", "true", "yes", "on"):
        return True
    if s in ("0", "false", "no", "off"):
        return False
    return None
