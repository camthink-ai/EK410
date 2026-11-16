#!/usr/bin/env python3
"""Bump firmware version across VERSION, main/version.h, and CHANGELOG stub.

Usage:
  python3 tools/bump_version.py 1.3.0
  python3 tools/bump_version.py --check          # verify files agree
  python3 tools/bump_version.py --show

SemVer: MAJOR.MINOR.PATCH (optional -prerelease suffix is not embedded in C macros).
After bumping, commit and tag:  git tag -a v1.3.0 -m "EK410 v1.3.0"
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
VERSION_FILE = ROOT / "VERSION"
VERSION_H = ROOT / "main" / "version.h"
CHANGELOG = ROOT / "CHANGELOG.md"

SEMVER = re.compile(r"^(\d+)\.(\d+)\.(\d+)$")


def read_version_file() -> str:
    return VERSION_FILE.read_text(encoding="utf-8").strip()


def parse(v: str) -> tuple[int, int, int]:
    m = SEMVER.match(v.strip())
    if not m:
        raise SystemExit(f"invalid SemVer (want X.Y.Z): {v!r}")
    return int(m.group(1)), int(m.group(2)), int(m.group(3))


def read_version_h() -> str:
    text = VERSION_H.read_text(encoding="utf-8")
    m = re.search(r'#define\s+EK410_VERSION_STRING\s+"([^"]+)"', text)
    if not m:
        raise SystemExit("EK410_VERSION_STRING not found in main/version.h")
    return m.group(1)


def write_version_h(major: int, minor: int, patch: int) -> None:
    ver = f"{major}.{minor}.{patch}"
    VERSION_H.write_text(
        f"""/**
 * @file version.h
 * @brief Firmware version — keep in sync with the repo root VERSION file.
 *
 * Update with:  python3 tools/bump_version.py X.Y.Z
 * Do not hand-edit FW_VERSION elsewhere; include this header.
 */
#ifndef EK410_VERSION_H_
#define EK410_VERSION_H_

#define EK410_VERSION_MAJOR {major}
#define EK410_VERSION_MINOR {minor}
#define EK410_VERSION_PATCH {patch}

/** Semantic version string shown in SoftAP / MQTT / getDevInfo. */
#define EK410_VERSION_STRING "{ver}"

/** Legacy alias used by older call sites. */
#define FW_VERSION EK410_VERSION_STRING

#endif /* EK410_VERSION_H_ */
""",
        encoding="utf-8",
    )


def ensure_changelog_heading(ver: str) -> None:
    if not CHANGELOG.exists():
        return
    text = CHANGELOG.read_text(encoding="utf-8")
    marker = f"## [{ver}]"
    if marker in text:
        return
    stub = (
        f"## [{ver}] — YYYY-MM-DD\n\n"
        f"### Added\n\n- \n\n"
        f"### Changed\n\n- \n\n"
        f"### Fixed\n\n- \n\n"
    )
    # Insert after the first H1 / intro block (before first ## [ or at top after title)
    m = re.search(r"^## \[", text, flags=re.M)
    if m:
        text = text[: m.start()] + stub + text[m.start() :]
    else:
        text = text.rstrip() + "\n\n" + stub
    CHANGELOG.write_text(text, encoding="utf-8")


def check() -> int:
    vf = read_version_file()
    vh = read_version_h()
    parse(vf)
    parse(vh)
    if vf != vh:
        print(f"MISMATCH: VERSION={vf}  version.h={vh}", file=sys.stderr)
        return 1
    print(f"OK {vf}")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("version", nargs="?", help="New version X.Y.Z")
    ap.add_argument("--check", action="store_true", help="Verify VERSION == version.h")
    ap.add_argument("--show", action="store_true", help="Print current version")
    ap.add_argument("--no-changelog", action="store_true", help="Do not stub CHANGELOG")
    args = ap.parse_args()

    if args.check:
        return check()
    if args.show or not args.version:
        print(read_version_file())
        return 0

    major, minor, patch = parse(args.version)
    ver = f"{major}.{minor}.{patch}"
    VERSION_FILE.write_text(ver + "\n", encoding="utf-8")
    write_version_h(major, minor, patch)
    if not args.no_changelog:
        ensure_changelog_heading(ver)
    print(f"bumped to {ver}")
    print("next: edit CHANGELOG.md, commit, then: git tag -a v%s -m 'EK410 v%s'" % (ver, ver))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
