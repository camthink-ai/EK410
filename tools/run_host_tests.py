#!/usr/bin/env python3
"""EK410 host-side regression tests — no device and no ESP-IDF needed.

    python3 tools/run_host_tests.py          # ~10 s; exit 1 on any failure

These guard what was wrong in 1.3.9 and is cheap to re-break:
  json_guard   JSON nesting-depth guard (compiles main/json_guard.c on the host)
  img_dither   native-frame dither (compiles main/img_dither.c on the host):
               layout identical to frame_convert, grey stays grey, tone kept
  img_dither_exif  the dither with the EXIF orientation: all eight give the
               upright frame, byte for byte, on both grids
  frame_conv   tools/frame_convert.py writes the panel's native frame layout
  qa           QA gate semantics (all-skipped is not ok, image version binding)
  web          embedded page in sync + deterministic + i18n consistent
  fonts        committed font tables well formed
  repo         version files agree, tools compile
  soak_dryrun  tools/soak_long.py against a simulated unit and broker: every
               kind of round runs, and each injected fault is found
A test whose prerequisite is missing (C compiler, Pillow, node) reports
"skip" — that is never counted as a pass. tools/build_release.sh runs this.
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TESTS = os.path.join(ROOT, "tools", "tests")


def run_c_test(work, name, sources, libs=()):
    """sources: file names under main/, or paths under tools/."""
    cc = os.environ.get("CC") or shutil.which("cc") or shutil.which("gcc")
    if not cc:
        return "skip", "no C compiler (cc/gcc) on PATH"
    exe = os.path.join(work, name + (".exe" if os.name == "nt" else ""))
    main_dir = os.path.join(ROOT, "main")
    files = [os.path.join(ROOT, s) if s.startswith("tools/") else os.path.join(main_dir, s)
             for s in sources]
    cmd = [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1",
           "-I", os.path.join(TESTS, "stub"), "-I", main_dir] + files + ["-o", exe] + list(libs)
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode != 0:
        return "fail", "compile failed: " + (p.stderr.strip().splitlines() or ["?"])[0][:160]
    p = subprocess.run([exe], capture_output=True, text=True)
    return ("fail" if p.returncode else "pass"), p.stdout.strip().splitlines()[-1] if p.stdout.strip() else "no output"


def run_py_test(name, env):
    p = subprocess.run([sys.executable, os.path.join(TESTS, name)], capture_output=True, text=True, env=env)
    lines = (p.stdout + p.stderr).strip().splitlines()
    last = lines[-1] if lines else "exit %s (no output)" % p.returncode
    status = "fail"
    if p.returncode == 0 and last.startswith("RESULT: pass"):
        status = "pass"
    elif p.returncode == 0 and last.startswith("RESULT: skip"):
        status = "skip"
    # The verdict first, then only the lines that explain it.
    keep = [last.replace("RESULT: ", "", 1)]
    keep += [l for l in lines[:-1] if re.match(r"^(FAIL|MISMATCH)|Error", l)]
    return status, "\n".join(keep)


def main():
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE="1")
    results = []
    with tempfile.TemporaryDirectory() as work:
        results.append(("json_guard", *run_c_test(
            work, "json_guard",
            ["json_guard.c", "tools/tests/stub/stub_cjson.c", "tools/tests/test_json_guard.c"])))
        results.append(("button_gesture", *run_c_test(
            work, "button_gesture",
            ["button_gesture.c", "tools/tests/test_button_gesture.c"])))
        results.append(("img_dither", *run_c_test(
            work, "img_dither", ["img_dither.c", "tools/tests/test_img_dither.c"], ["-lm"])))
        results.append(("img_dither_exif", *run_c_test(
            work, "img_dither_exif", ["img_dither.c", "tools/tests/test_img_dither_orient.c"],
            ["-lm"])))
        for name in ("test_frame_convert.py", "test_img_dither_layout.py", "test_qa_lib.py", "test_web.py",
                     "test_fonts.py", "test_repo.py", "test_soak_dryrun.py"):
            results.append((name[5:-3], *run_py_test(name, env)))

    width = max(len(n) for n, _, _ in results)
    counts = {"pass": 0, "fail": 0, "skip": 0}
    for name, status, detail in results:
        counts[status] += 1
        for i, line in enumerate(detail.split("\n")):
            print(("  %-4s %-*s  " % (status.upper(), width, name) if i == 0 else " " * (width + 12)) + line)
    print("host tests: %d passed, %d failed, %d skipped" % (counts["pass"], counts["fail"], counts["skip"]))
    if counts["fail"]:
        return 1
    if not counts["pass"]:
        print("nothing could run — treat as a failure, not a pass", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
