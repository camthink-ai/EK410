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


def firmware_app_version(path: Path) -> str:
    """Version string from the image's esp_app_desc. PROJECT_VER is bound to
    the repo VERSION by the top-level CMakeLists, so this is the release's
    own version. "" when the file is not an ESP app image."""
    try:
        with path.open("rb") as f:
            head = f.read(0x20 + 80)
    except OSError:
        return ""
    desc = head[0x20:]
    if len(desc) < 80 or head[:1] != b"\xE9" or desc[:4] != b"\x32\x54\xCD\xAB":
        return ""
    return desc[16:48].split(b"\x00")[0].decode("ascii", "replace")


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
        "app_version": firmware_app_version(path),
    }
