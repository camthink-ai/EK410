#!/usr/bin/env python3
"""QA gate semantics (tools/qa_lib, tools/qa_gate.py): no device needed.

- An all-skipped / all-invalid run is not ok, a single pass with no failure is.
- The image's embedded esp_app_desc version is read from a synthetic image.
- qa_gate.py exposes the --mqtt-password pass-through.

Run by tools/run_host_tests.py.
"""
import os
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

TOOLS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, TOOLS)
from qa_lib.firmware import firmware_app_version  # noqa: E402
from qa_lib.result import CaseResult, Status, SuiteReport  # noqa: E402


def main():
    failed = []

    def check(name, ok):
        if not ok:
            failed.append(name)

    r = SuiteReport("smoke")
    r.add(CaseResult("a", Status.SKIPPED)); r.add(CaseResult("b", Status.INVALID))
    check("all skipped/invalid must not be ok", r.ok is False)
    r.add(CaseResult("c", Status.PASS))
    check("one pass, nothing failed must be ok", r.ok is True)
    r.add(CaseResult("d", Status.FAIL_DEVICE))
    check("a device failure must not be ok", r.ok is False)
    d = r.to_dict()
    check("report carries firmware_version", "firmware_version" in d and "firmware_version" in SuiteReport.__dataclass_fields__)

    # Synthetic ESP app image: image header (0x20 bytes) + esp_app_desc.
    desc = struct.pack("<IIII", 0xABCD5432, 0, 0, 0)  # magic_word, secure_version, reserv1[2] -> 16 bytes
    desc += b"9.9.9".ljust(32, b"\0") + b"ek410".ljust(32, b"\0") + b"00:00:00".ljust(16, b"\0")
    desc += b"Jan  1 2026".ljust(16, b"\0") + b"v5.5.6".ljust(32, b"\0") + b"\0" * 32 + b"\0" * 80
    with tempfile.TemporaryDirectory() as tmp:
        good = Path(tmp) / "app.bin"
        good.write_bytes(b"\xE9" + b"\0" * 31 + desc)
        check("app version read from a valid image", firmware_app_version(good) == "9.9.9")
        boot = Path(tmp) / "boot.bin"
        boot.write_bytes(b"\xE9" + b"\0" * 200)
        check("non-app image yields ''", firmware_app_version(boot) == "")
        check("missing file yields ''", firmware_app_version(Path(tmp) / "nope.bin") == "")

    env = dict(os.environ, PYTHONDONTWRITEBYTECODE="1")
    p = subprocess.run([sys.executable, os.path.join(TOOLS, "qa_gate.py"), "--help"],
                       capture_output=True, text=True, env=env)
    check("qa_gate --help exits 0", p.returncode == 0)
    check("qa_gate advertises --mqtt-password", "--mqtt-password" in p.stdout)
    p = subprocess.run([sys.executable, os.path.join(TOOLS, "qa_gate.py"), "--tier", "smoke"],
                       capture_output=True, text=True, env=env)
    check("qa_gate without --port is a setup error (exit 2)", p.returncode == 2)

    if failed:
        print("RESULT: fail - " + "; ".join(failed))
        return 1
    print("RESULT: pass - %d checks" % 11)
    return 0


if __name__ == "__main__":
    sys.exit(main())
