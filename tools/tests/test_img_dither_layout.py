#!/usr/bin/env python3
"""main/img_dither.c must write the panel's native frame layout.

Cross-check: tools/frame_convert.py's pack_native() is the layout the firmware
streams (itself cross-checked against stream_render_unlocked by
test_frame_convert.py). A picture made of the six ink colours dithers to
itself, so the C module's frame must equal pack_native() byte for byte — on
the full grid and on the 800x600 grid.

Run by tools/run_host_tests.py. Needs a C compiler and Pillow (skipped
otherwise).
"""
import os
import shutil
import subprocess
import sys
import tempfile

TOOLS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROOT = os.path.dirname(TOOLS)
sys.path.insert(0, TOOLS)

# (r, g, b, code): the firmware's soft ink colours
INKS = [(22, 22, 22, 0x0), (248, 248, 242, 0x1), (228, 198, 42, 0x2),
        (188, 42, 40, 0x3), (42, 72, 158, 0x5), (42, 122, 68, 0x6)]
BX = (0, 534, 1066, 1600)   # even, so both grids put the same dot on the edge
BY = (0, 600, 1200)


def block(lx, ly):
    col = 0 if lx < BX[1] else (1 if lx < BX[2] else 2)
    return INKS[(0 if ly < BY[1] else 3) + col]


def main():
    cc = os.environ.get("CC") or shutil.which("cc") or shutil.which("gcc")
    try:
        import frame_convert as fc
    except ImportError:
        print("RESULT: skip - Pillow missing")
        return 0
    if not cc:
        print("RESULT: skip - no C compiler")
        return 0

    with tempfile.TemporaryDirectory() as work:
        exe = os.path.join(work, "img_dither_cli")
        build = subprocess.run(
            [cc, "-std=c11", "-O2", "-I", os.path.join(ROOT, "main"),
             os.path.join(ROOT, "main", "img_dither.c"),
             os.path.join(TOOLS, "tests", "img_dither_cli.c"), "-o", exe, "-lm"],
            capture_output=True, text=True)
        if build.returncode:
            print("FAIL compile: " + (build.stderr.strip().splitlines() or ["?"])[0][:160])
            print("RESULT: fail - compile")
            return 1

        rows = []
        for ly in range(1200):
            row = bytearray()
            for c in range(3):
                row += bytes(block(BX[c], ly)[:3]) * (BX[c + 1] - BX[c])
            rows.append(bytes(row))
        rgb = b"".join(rows)
        expected = fc.pack_native(lambda lx, ly: block(lx, ly)[3])
        assert len(expected) == 960000 and len(rgb) == 1600 * 1200 * 3

        bad = 0
        for gw, gh in ((1600, 1200), (800, 600)):
            run = subprocess.run([exe, "1600", "1200", str(gw), str(gh)], input=rgb,
                                 capture_output=True)
            if run.returncode:
                print(f"FAIL grid {gw}x{gh}: cli exit {run.returncode}")
                bad += 1
                continue
            got = run.stdout
            diff = sum(1 for a, b in zip(got, expected) if a != b)
            if len(got) != len(expected) or diff:
                print(f"MISMATCH grid {gw}x{gh}: {diff} of {len(expected)} bytes differ")
                bad += 1
        # the comparison must be able to fail: one flipped byte is caught
        probe = bytes([expected[0] ^ 0x10]) + expected[1:]
        assert sum(1 for a, b in zip(probe, expected) if a != b) == 1
        if bad:
            print("RESULT: fail - native frame layout differs from frame_convert")
            return 1
        print("RESULT: pass - 960000 bytes identical to frame_convert.pack_native on both grids")
        return 0


if __name__ == "__main__":
    sys.exit(main())
