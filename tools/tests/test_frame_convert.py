#!/usr/bin/env python3
"""tools/frame_convert.py must write the panel's native frame layout.

Cross-check: the tool maps logical -> native (nx = 1199 - ly, ny = lx); this
test decodes its output with a literal transcription of what the firmware
sends (stream_render_unlocked in main/app_epd.c, forward mapping lx = ny,
ly = 1199 - nx). The two directions were written independently, so agreement
is a real check — and a deliberately wrong layout must be detected.

Run by tools/run_host_tests.py. Needs Pillow (skipped otherwise).
"""
import os
import sys
import tempfile

TOOLS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, TOOLS)


def native_to_logic(nx, ny):  # main/app_epd.c: lx = ny, ly = 1199 - nx
    return ny, 1199 - nx


def firmware_stream(code):
    """Byte for byte what stream_render_unlocked() sends for `code(lx, ly)`."""
    out = bytearray()
    for csx in range(2):
        for ny in range(1600):
            for b in range(300):
                nx = csx * 600 + b * 2
                hi = code(*native_to_logic(nx, ny))
                lo = code(*native_to_logic(nx + 1, ny))
                out.append(((hi & 0x0F) << 4) | (lo & 0x0F))
    return bytes(out)


def main():
    try:
        import frame_convert as fc
        from PIL import Image
    except ImportError as e:
        print("RESULT: skip - Pillow / frame_convert unavailable (%s)" % e)
        return 0

    col = {"black": (0, 0, 0), "red": (255, 0, 0), "green": (0, 255, 0),
           "blue": (0, 0, 255), "yellow": (255, 255, 0)}
    code_of = {v: c for c, v in fc.PALETTE}
    w, h = fc.PANEL_W, fc.PANEL_H
    probes = [(0, 0), (w - 1, 0), (0, h - 1), (w - 1, h - 1), (799, 599), (800, 600)]

    def logical(lx, ly):
        if (lx, ly) in probes:
            return code_of[col["yellow"]]
        if lx < w // 2 and ly < h // 2:
            return code_of[col["black"]]
        if lx >= w // 2 and ly < h // 2:
            return code_of[col["red"]]
        if lx < w // 2:
            return code_of[col["green"]]
        return code_of[col["blue"]]

    tool = fc.pack_native(logical)
    ref = firmware_stream(logical)
    if len(tool) != 960000 or len(ref) != 960000:
        print("RESULT: fail - frame length %d / %d (expect 960000)" % (len(tool), len(ref)))
        return 1
    diff = sum(1 for a, b in zip(tool, ref) if a != b)
    if diff:
        print("RESULT: fail - %d of 960000 bytes differ from the firmware writer" % diff)
        return 1
    # Negative control: a wrong layout (halves swapped, not rotated) must not pass.
    wrong = firmware_stream(lambda lx, ly: logical(1199 - ly, lx) if False else logical(lx, ly))
    swapped = ref[480000:] + ref[:480000]
    if swapped == ref or sum(1 for a, b in zip(swapped, ref) if a != b) == 0:
        print("RESULT: fail - negative control is indistinguishable")
        return 1
    # End to end: image file -> tool -> same bytes.
    img = Image.new("RGB", (w, h))
    px = img.load()
    for ly in range(h):
        for lx in range(w):
            px[lx, ly] = (col["yellow"] if (lx, ly) in probes else
                          col["black"] if lx < w // 2 and ly < h // 2 else
                          col["red"] if lx >= w // 2 and ly < h // 2 else
                          col["green"] if lx < w // 2 else col["blue"])
    with tempfile.TemporaryDirectory() as tmp:
        src, dst = os.path.join(tmp, "q.png"), os.path.join(tmp, "q.bin")
        img.save(src)
        fc.convert(src, dst)
        data = open(dst, "rb").read()
    if data != ref:
        bad = sum(1 for a, b in zip(data, ref) if a != b)
        print("RESULT: fail - convert() output differs from the firmware writer (%d bytes)" % bad)
        return 1
    print("RESULT: pass - 960000 bytes identical to the firmware writer; negative control detected; convert() end-to-end ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
