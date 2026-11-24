#!/usr/bin/env python3
"""Repository hygiene that needs no toolchain: version files agree, every
Python tool compiles, every shell script parses.

Run by tools/run_host_tests.py.
"""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def main():
    failed = []
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE="1")

    p = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "bump_version.py"), "--check"],
                       capture_output=True, text=True, env=env)
    if p.returncode != 0:
        failed.append("VERSION != main/version.h: %s" % (p.stdout + p.stderr).strip())

    for base, _, files in os.walk(os.path.join(ROOT, "tools")):
        if "gpio_scan" in base or "__pycache__" in base:
            continue
        for f in files:
            if f.endswith(".py"):
                try:  # compile() in memory: no __pycache__ written into the tree
                    compile(open(os.path.join(base, f), encoding="utf-8").read(), f, "exec")
                except SyntaxError as e:
                    failed.append("syntax: %s (line %s)" % (f, e.lineno))
            elif f.endswith(".sh"):
                r = subprocess.run(["bash", "-n", os.path.join(base, f)], capture_output=True, text=True)
                if r.returncode != 0:
                    failed.append("bash -n: %s (%s)" % (f, r.stderr.strip()[:80]))

    for name in ("VERSION", "main/version.h"):
        if not os.path.isfile(os.path.join(ROOT, name)):
            failed.append("missing " + name)

    if failed:
        print("RESULT: fail - " + "; ".join(failed))
        return 1
    print("RESULT: pass - version files agree; tools compile")
    return 0


if __name__ == "__main__":
    sys.exit(main())
