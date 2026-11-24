#!/usr/bin/env python3
"""The committed font tables are well formed.

main/font_hz16.c — N code points, strictly increasing (binary search relies
on it), N bitmaps of exactly 32 bytes; only U+3000 (ideographic space) blank.
main/font8x16.h — 95 glyphs of 16 bytes, only the space is blank.

Run by tools/run_host_tests.py.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def main():
    failed = []

    def check(name, ok, detail=""):
        if not ok:
            failed.append(name + (" (" + detail + ")" if detail else ""))

    c = open(os.path.join(ROOT, "main", "font_hz16.c"), encoding="utf-8").read()
    n = int(re.search(r"#define HZ16_N (\d+)", c).group(1))
    us = [int(x, 16) for x in re.findall(r"0x[0-9A-Fa-f]{4}", c.split("s_hz_u[HZ16_N]")[1].split("};")[0])]
    body = c.split("s_hz_bits[HZ16_N][32]")[1].split("};")[0]
    bits = [[int(x, 16) for x in re.findall(r"0x[0-9A-Fa-f]{2}", row)] for row in re.findall(r"\{([^{}]*)\}", body)]
    check("HZ16_N == code point count == bitmap count", n == len(us) == len(bits), "%d/%d/%d" % (n, len(us), len(bits)))
    check("code points strictly increasing", all(a < b for a, b in zip(us, us[1:])))
    check("every CJK bitmap is 32 bytes", all(len(b) == 32 for b in bits))
    blank = [u for u, b in zip(us, bits) if not any(b) if u != 0x3000]
    check("no blank CJK glyph besides U+3000", not blank, "blank: %s" % [hex(u) for u in blank[:6]])
    check("GB2312 repertoire (7445)", n == 7445, "n=%d" % n)
    check("CJK table states its OFL source", "SIL OFL" in c and "Noto Sans CJK" in c)

    a = open(os.path.join(ROOT, "main", "font8x16.h"), encoding="utf-8").read()
    rows = [[int(x, 16) for x in re.findall(r"0x[0-9A-Fa-f]{2}", r)] for r in re.findall(r"\{([^{}]*)\}", a.split("font8x16[")[1])]
    check("ASCII table: 95 glyphs of 16 bytes", len(rows) == 95 and all(len(r) == 16 for r in rows), "%d" % len(rows))
    check("ASCII table: only the space is blank", not any(rows[0]) and all(any(r) for r in rows[1:]))
    check("ASCII table states its OFL source", "SIL OFL" in a and "Noto Sans Mono" in a)

    if failed:
        print("RESULT: fail - " + "; ".join(failed))
        return 1
    print("RESULT: pass - %d CJK glyphs, %d ASCII glyphs" % (n, len(rows)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
