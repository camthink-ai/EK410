"""Firmware artifact identity for QA reports."""
from __future__ import annotations

import hashlib
from pathlib import Path
from typing import Any


def firmware_sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def firmware_meta(path: Path) -> dict[str, Any]:
    if not path.is_file():
        return {"path": str(path), "present": False}
    sha = firmware_sha256(path)
    return {
        "path": str(path.resolve()),
        "present": True,
        "bytes": path.stat().st_size,
        "sha256": sha,
        "sha256_12": sha[:12],
    }
