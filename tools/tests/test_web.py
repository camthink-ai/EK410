#!/usr/bin/env python3
"""The embedded web page (main/www/index.html -> main/www_html.h).

- Generating twice gives identical output (no timestamps / randomness).
- The committed www_html.h embeds exactly today's index.html + qrcodegen.min.js
  (compared after decompression, so a different zlib cannot cause a false alarm).
- zh / en string tables define the same keys, and every key the page uses exists.
- With node available: both inline script blocks parse.

Run by tools/run_host_tests.py.
"""
import gzip
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import gen_www  # noqa: E402


def main():
    failed = []

    def check(name, ok, detail=""):
        if not ok:
            failed.append(name + (" (" + detail + ")" if detail else ""))

    with tempfile.TemporaryDirectory() as tmp:
        a, b = os.path.join(tmp, "a.h"), os.path.join(tmp, "b.h")
        gen_www.render(a)
        gen_www.render(b)
        check("gen_www is deterministic", open(a, "rb").read() == open(b, "rb").read())

        # Decompress what the committed header embeds and compare with a fresh build.
        hdr = open(os.path.join(ROOT, "main", "www_html.h"), encoding="utf-8").read()
        body = hdr.split("INDEX_HTML_GZ[] = {", 1)[1].split("};", 1)[0]
        embedded = gzip.decompress(bytes(int(x, 16) for x in re.findall(r"0x[0-9a-fA-F]{2}", body)))
        check("committed www_html.h matches index.html (+ qrcodegen)",
              embedded == gen_www.build_html().encode("utf-8"),
              "run: python3 tools/gen_www.py")

    src = open(os.path.join(ROOT, "main", "www", "index.html"), encoding="utf-8").read()
    m = re.search(r"const I18N = \{\s*zh:\{(.*?)\},\s*en:\{(.*?)\n\}", src, flags=re.S)
    check("I18N table found", bool(m))
    if m:
        key = lambda block: set(re.findall(r'"([A-Za-z0-9_.]+)"\s*:', block))
        zh, en = key(m.group(1)), key(m.group(2))
        check("zh/en define the same keys", zh == en, "only-zh=%s only-en=%s" % (sorted(zh - en)[:4], sorted(en - zh)[:4]))
        used = set(re.findall(r"\bt\('([A-Za-z0-9_.]+)'", src))
        used |= set(re.findall(r'data-i18n(?:-placeholder)?="([A-Za-z0-9_.]+)"', src))
        used |= set(re.findall(r"setOverviewChip\([^)]*?'([a-z]+\.[A-Za-z0-9_.]+)'", src))
        missing = sorted(k for k in used if "." in k and k not in zh)
        check("every key the page uses is defined", not missing, "missing: %s" % missing[:8])

    node = subprocess.run(["node", "--version"], capture_output=True, text=True)
    if node.returncode != 0:
        print("  node not found - script syntax check skipped")
    else:
        html = gen_www.build_html()
        blocks = re.findall(r"<script(?:\s[^>]*)?>(.*?)</script>", html, flags=re.S)
        bad = 0
        with tempfile.TemporaryDirectory() as tmp:
            for i, s in enumerate(blocks):
                p = os.path.join(tmp, "block%d.js" % i)
                open(p, "w", encoding="utf-8").write(s)
                r = subprocess.run(["node", "--check", p], capture_output=True, text=True)
                if r.returncode != 0:
                    bad += 1
                    print(r.stderr[:300])
        check("all inline script blocks parse (node --check)", bad == 0, "%d of %d blocks" % (bad, len(blocks)))

    if failed:
        print("RESULT: fail - " + "; ".join(failed))
        return 1
    print("RESULT: pass - embedded page in sync, deterministic, i18n consistent, scripts parse")
    return 0


if __name__ == "__main__":
    sys.exit(main())
