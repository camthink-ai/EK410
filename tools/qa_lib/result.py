"""Machine-readable QA result model."""
from __future__ import annotations

import json
import time
from dataclasses import asdict, dataclass, field
from enum import Enum
from pathlib import Path
from typing import Any


class Status(str, Enum):
    PASS = "pass"
    FAIL_DEVICE = "fail_device"
    FAIL_FIXTURE = "fail_fixture"
    SKIPPED = "skipped"
    INVALID = "invalid"  # missing measurement / disconnected instrument


@dataclass
class CaseResult:
    name: str
    status: Status
    detail: str = ""
    duration_s: float = 0.0
    meta: dict[str, Any] = field(default_factory=dict)

    def to_dict(self) -> dict[str, Any]:
        d = asdict(self)
        d["status"] = self.status.value
        return d


@dataclass
class SuiteReport:
    tier: str
    firmware_sha256: str = ""
    firmware_path: str = ""
    device_id: str = ""
    host: str = ""
    port: str = ""
    started_at: float = field(default_factory=time.time)
    finished_at: float = 0.0
    cases: list[CaseResult] = field(default_factory=list)

    def add(self, case: CaseResult) -> None:
        self.cases.append(case)

    def counts(self) -> dict[str, int]:
        out = {s.value: 0 for s in Status}
        for c in self.cases:
            out[c.status.value] += 1
        return out

    @property
    def ok(self) -> bool:
        """Pass if no device/fixture failures. skipped/invalid do not fail the gate."""
        return not any(
            c.status in (Status.FAIL_DEVICE, Status.FAIL_FIXTURE) for c in self.cases
        )

    def to_dict(self) -> dict[str, Any]:
        self.finished_at = self.finished_at or time.time()
        return {
            "tier": self.tier,
            "firmware_sha256": self.firmware_sha256,
            "firmware_path": self.firmware_path,
            "device_id": self.device_id,
            "host": self.host,
            "port": self.port,
            "started_at": self.started_at,
            "finished_at": self.finished_at,
            "elapsed_s": self.finished_at - self.started_at,
            "counts": self.counts(),
            "ok": self.ok,
            "cases": [c.to_dict() for c in self.cases],
        }

    def write_json(self, path: Path) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(self.to_dict(), indent=2, ensure_ascii=False) + "\n")

    def write_jsonl(self, path: Path) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open("w", encoding="utf-8") as f:
            for c in self.cases:
                row = c.to_dict()
                row["tier"] = self.tier
                row["firmware_sha256"] = self.firmware_sha256
                f.write(json.dumps(row, ensure_ascii=False) + "\n")
