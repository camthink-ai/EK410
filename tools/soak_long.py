#!/usr/bin/env python3
"""EK410 long soak — business rhythm on one unit, unattended.

Cycles what a unit does all day — URL pictures, canvases and text screens, a
raw frame upload now and then — while MQTT telemetry keeps running; samples
heap and task stacks after every round, reads back the frame the device
saved, watches the serial log for faults and defect markers, and appends one
JSON line per round, so the run can be read at any time.

What a run exercises:

  pictures       served from this computer: JPEG at several sizes and shapes
                 (landscape, portrait, a banner, a tiny icon, one with an
                 EXIF orientation tag), PNG (truecolour, six-colour,
                 grayscale, with transparency), a file exactly on the 700 KB
                 limit, a raw frame, redirects, chunked and length-less
                 transfers
  refused        files the firmware must refuse (over the limit, truncated,
                 progressive and grayscale JPEG — the ROM decoder takes
                 three-component baseline only), with the reason it logs
  server faults  an HTTP 404, a transfer cut in half, a server that stalls, a
                 self-signed certificate: each must fail cleanly, with the
                 expected reason, and leave the unit healthy
  content        a dashboard canvas (text, boxes, a QR code) and a long text
                 screen — what most units really show
  concurrency    while a picture is being displayed: console-style status
                 polling and the console page, a proxyImage (waits behind the
                 display), a second display request (queued, shown next), and
                 an uploadFrame — which must be refused at once ("panel
                 busy"): until 1.3.10 it waited for the panel with its body
                 unread, and the unit lost Wi-Fi every time — and the frame
                 on the panel must be the one the last request asked for
  frames         after every round the saved frame is read back. An upload or
                 a raw frame must be byte-identical to what was sent; a canvas
                 or text screen must come out as it did the first time, never
                 blank; every display must have saved a frame of its own.
                 Whether a picture gives the same frame every time is only
                 recorded: the decode resolution depends on the largest free
                 block at that moment, so one file can come out at two
                 sharpnesses.
  https          optionally a public HTTPS image (--https-url), the only TLS
                 path that succeeds
  broker         with --broker-secret, the unit's own credentials taken from it
                 by tools/broker_secret.py: a second client on the broker the
                 unit publishes to. After every round a telemetry has to
                 arrive that tells the broker what the unit shows (frame CRC,
                 a preview of the stated size); every --mqtt-every rounds the
                 command goes over the broker instead of HTTP — acknowledged
                 "queued", then "ok", or "error" with the firmware's reason;
                 every --ping-every rounds a ping goes the same way
  provisioning   somebody may open BLE provisioning or the hotspot on the
                 unit while it runs (a button, the app). That round is not
                 judged. What is: the unit is a client again afterwards — on
                 1.3.10 it was not after BLE, until a reboot
  ota            optionally, every --ota-every-hours, the same firmware is
                 uploaded again (--image): the unit must come back in the
                 other slot

It never changes the device's configuration. `qa_gate.py --tier soak` is the
stress rig (SoftAP cycles, broker flips, wrong Wi-Fi password); this is the
business-rhythm counterpart. Not covered here: sleep/wake cycles, broker or
access-point outages, what a phone does during provisioning (only the unit's
recovery afterwards).

    python3 tools/soak_long.py --host 192.168.31.236 --out ~/ek410-soak \\
        --port /dev/cu.wchusbserialXXXX --hours 72 --image dist/ek410.bin \\
        --https-url https://HOST/pic.jpg --https-every 37 --ota-every-hours 24 \\
        --broker-secret ~/.ek410-broker.json

With --port the serial log is kept: it is the only way to tell an intended
failure from any other, because the firmware logs its reason there
("url frame fail: …"). Needs Pillow once, to generate the image set into
<out>/www (the ESP-IDF Python environment has it), and `openssl` for the
self-signed certificate. On macOS the script re-executes itself under
`caffeinate -i -s` so the host does not idle-sleep (`--no-caffeinate` turns
that off); a closed lid still sleeps a laptop — the run then goes on, reports
how long the host was away and does not judge the rounds it slept through.
Create <out>/STOP to end a run early; `summary.json` is rewritten after every
round. Exit code 0 only when the run met every criterion.
"""

from __future__ import annotations

import argparse
import base64
import collections
import functools
import hashlib
import http.client
import http.server
import json
import os
import platform
import re
import shutil
import socket
import ssl
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request
import zlib
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))

IMAGE_LIMIT = 700 * 1024      # main/app_epd.c IMG_URL_IMAGE_MAX_BYTES
RAW_FRAME_BYTES = 960000
OVER_LIMIT = "image >700KB limit"
DISPLAY_TIMEOUT_S = 300       # a queued second display follows the picture (~50 s)
UPLOAD_TIMEOUT_S = 330        # behind a refresh uploadFrame first waits for the
                             # panel lock (up to 180 s), then streams and refreshes
MQTT_RESUME_S = 75
STALL_S = 75                 # longer than the firmware's 60 s HTTP read timeout
TELEMETRY_S = 30             # main/mqtt.c TELEMETRY_PERIOD_US
ACK_S = 10                   # "queued" for a command on the broker
PROV_S = 90                  # back as a client this long after leaving provisioning
PREVIEW_BMP = 54 + 1024      # 8-bit BMP header + palette; the pixels follow
IN_WHITE, IN_RED = 0x1, 0x3
FAULT_MARKERS = (
    "Guru Meditation", "abort() was called", "Stack protection fault",
    "assert failed", "Interrupt wdt timeout", "Task watchdog got triggered",
    "CORRUPT HEAP", "Prohibited", "Backtrace:", "Rebooting...", "Brownout",
)
# No crash, but a feature that silently did not happen (the 72 h run on
# 2c5ba1cc5801 logged "preview BMP OOM" after every URL raw frame: telemetry
# went out without a preview each time, and nothing in the verdicts showed it).
DEFECT_MARKERS = (
    "preview BMP OOM", "preview b64 OOM", "preview b64 encode fail",
    "capture OOM", "preview: no buffer", "display incomplete",
    "upload size mismatch", "driver IC status not ready",
    # …and a saved frame that is not the one just shown: telemetry would
    # carry the wrong preview and the next wake-up would replay it.
    "capture incomplete", "capture overflow", "erase fail", "frame write fail",
    "hdr write fail", "stream OOM",
)
ASSET_REF = re.compile(r'(?:src|href)="([^"#]+)"')

# name, how it is built or served, what the device is expected to do, and for
# an intended failure the reason(s) the firmware logs ("url frame fail: …").
# The pictures are smooth — the panel stands on a bench where people look at
# it. What matters to the device is the size of the body, so the big files are
# padded after EOI / IEND to the sizes that squeeze its memory: the decoders
# stop at the end marker, the body still takes the room. limit.jpg sits
# exactly on the limit. dense.jpg is never displayed: it is real data all the
# way, so the server can cut or stall inside it.
IMAGE_SET = [
    ("portrait.jpg", ("jpeg", 1, 1200, 1600, 90, 0), "display", None),
    ("banner.jpg", ("jpeg", 2, 2400, 600, 90, 0), "display", None),
    ("icon.png", ("png", "RGB", 64, 64, 0), "display", None),
    ("exif.jpg", ("exif", 1600, 1200), "display", None),
    ("alpha.png", ("alpha", 1600, 1200), "display", None),
    ("chunked.jpg", ("serve", "chunked", "/jpeg.jpg"), "display", None),
    ("nolength.jpg", ("serve", "nolength", "/jpeg.jpg"), "display", None),
    ("jpeg.jpg", ("jpeg", 0, 1600, 1200, 90, 0), "display", None),
    ("photo.jpg", ("jpeg", 1, 1600, 1200, 92, 620 * 1024), "display", None),
    ("large.jpg", ("jpeg", 2, 2400, 1800, 90, 540 * 1024), "display", None),
    ("chart.png", ("png", "RGB", 2400, 1800, 0), "display", None),
    ("six.png", ("png", "P", 1600, 1200, 420 * 1024), "display", None),
    ("gray.png", ("png", "L", 800, 600, 150 * 1024), "display", None),
    ("limit.jpg", ("pad", "photo.jpg", IMAGE_LIMIT), "display", None),
    ("dense.jpg", ("noise", 1024, 768, 80), "source", None),
    ("raw.bin", ("raw", "jpeg.jpg"), "display", None),
    ("redir.jpg", ("redirect", "/jpeg.jpg"), "display", None),
    ("redir-raw.bin", ("redirect", "/raw.bin"), "display", None),
    ("oversize.jpg", ("pad", "photo.jpg", IMAGE_LIMIT + 1024), "reject",
     OVER_LIMIT),
    ("trunc.png", ("truncate", "gray.png", 3000), "reject", "PNG decode fail"),
    ("progressive.jpg", ("progressive", "photo.jpg"), "reject",
     "progressive JPEG not supported"),
    # The JPEG decoder in the C61 ROM takes three-component images only: a
    # single-component (grayscale) JPEG is refused like a progressive one.
    # 1.3.11 says which of the two it was.
    ("gray.jpg", ("jpeggray", 1600, 1200), "reject", "grayscale JPEG not supported"),
]
# What a server can do wrong. Each must fail cleanly with one of the reasons.
FAULT_SET = [
    ("http-404.jpg", ("status", 404), "HTTP status 404"),
    ("cut.jpg", ("cut", "/dense.jpg", 0.5),
     ["http read error", "JPEG decode fail", "empty download"]),
    ("stall.jpg", ("stall", "/dense.jpg"), ["http read error", "URL net timeout"]),
    ("selfsigned.jpg", ("selfsigned", "/jpeg.jpg"), "http open/TLS fail"),
]
TO_DISPLAY = [entry for entry in IMAGE_SET if entry[2] == "display"]
TO_REJECT = [entry for entry in IMAGE_SET if entry[2] == "reject"]
SERVED = ("redirect", "serve")             # behaviour tables, no file of their own
NOT_STORED = SERVED + ("raw",)             # …and the raw frame: not bound by the limit
RAW_FRAMES = ("raw.bin", "redir-raw.bin")
BURST_PATTERNS = ("poll", "proxy", "second", "upload")
FRAME_KEYS = {"upload": "upload", "canvas": "canvas", "text": "text", "https": "https"}
BURST_DETAIL = ("console", "proxy", "second", "upload")

# A dashboard, as the web console sends it, and a screen of text.
DASH_LINES = [
    "车间 A 线产量 1,240 件 · 达标率 98.2%",
    "Room temperature 23.5 C · humidity 41 %",
    "今日待办 3 项 · 完成 12 项 · 异常 0 项",
    "Next service 2026-12-01 09:00 · spare parts OK",
    "Batch EK-4102-07 running · step 4 / 6",
    "库存预警: 轴承 6204 剩余 18 套, 请补货",
]
CANVAS = {"bg": "white", "items": [
    {"type": "rect", "x": 40, "y": 36, "w": 1520, "h": 1128, "thickness": 4,
     "fill": False, "color": "black"},
    {"type": "text", "x": 80, "y": 64, "text": "EK410 车间看板 dashboard", "scale": 4,
     "color": "black"},
    {"type": "rect", "x": 80, "y": 210, "w": 1440, "h": 56, "fill": True, "color": "blue"},
] + [
    {"type": "text", "x": 96, "y": 310 + i * 112, "text": line, "scale": 2,
     "color": ("black", "red", "green", "black", "blue", "red")[i]}
    for i, line in enumerate(DASH_LINES)
] + [
    {"type": "rect", "x": 96, "y": 1010, "w": 700, "h": 26, "fill": True, "color": "green"},
    {"type": "qr", "x": 1120, "y": 640, "size": 420,
     "text": "https://www.camthink.ai/?device=ek410-soak"},
]}
TEXT_SCREEN = {
    "color": "black", "bg": "white", "scale": 2,
    "text": "\n".join([
        "EK410 长文本 soak · 第 1 行",
        "The quick brown fox jumps over the lazy dog. 0123456789",
        "第 3 行：中文与 ASCII 混排，标点：，。！？；",
        "Line 4 keeps going for a while to show how a paragraph wraps "
        "when it is wider than the panel is.",
        "第 5 行 · 五",
        "------ ------ ------",
        "第七行是最后一条消息，感谢阅读。",
    ]),
}


def log(msg: str) -> None:
    print(time.strftime("%H:%M:%S "), msg, flush=True)


def finite(values) -> list:
    return [v for v in values if isinstance(v, (int, float))]


def _code(data: bytes | None):
    try:
        obj = json.loads(data or b"null")
    except ValueError:
        return None
    return obj.get("code") if isinstance(obj, dict) else None


def _alive(pid: int) -> bool:
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except OSError:
        pass
    return True


class Clock:
    """How long the host was frozen for (lid closed): wall time that passed
    while the monotonic clock — which stops during sleep — did not."""

    def __init__(self):
        self.wall, self.mono = time.time(), time.monotonic()
        self.frozen = 0.0

    def check(self) -> float:
        """Seconds the host just slept for; 0 when it did not."""
        wall, mono = time.time(), time.monotonic()
        gap = (wall - self.wall) - (mono - self.mono)
        self.wall, self.mono = wall, mono
        if gap > 30:
            self.frozen += gap
            return gap
        return 0.0


# ---------- image set ----------

# sky top, sky at the horizon, far hills, near hills, water: the tones between
# the six inks are what the dither has to deal with, so none of them is one
_LANDSCAPES = (
    ((96, 150, 214), (214, 226, 236), (96, 140, 104), (44, 96, 62), (64, 118, 166)),   # day
    ((84, 92, 168), (244, 176, 120), (128, 96, 110), (58, 62, 78), (150, 110, 124)),   # sunset
    ((150, 196, 206), (232, 232, 214), (120, 150, 96), (70, 112, 60), (86, 142, 140)),  # morning
)


def _art(w: int, h: int, variant: int):
    """A smooth landscape: gradients, flat hills, a sun, a boat, a label."""
    from PIL import Image, ImageDraw
    top, horizon, far, near, water = _LANDSCAPES[variant % len(_LANDSCAPES)]
    strip = Image.new("RGB", (1, h))
    for y in range(h):
        t = y / max(h - 1, 1)
        if t < 0.5:                       # sky
            k = t / 0.5
            c = tuple(int(a + (b - a) * k) for a, b in zip(top, horizon))
        else:                             # water
            k = (t - 0.5) / 0.5
            c = tuple(int(a + (b - a) * k) for a, b in zip(horizon, water))
        strip.putpixel((0, y), c)
    im = strip.resize((w, h))
    d = ImageDraw.Draw(im)
    d.ellipse((w * 0.70, h * 0.08, w * 0.86, h * 0.08 + w * 0.16), fill=(246, 214, 92))
    d.polygon([(0, h * 0.50), (w * 0.22, h * 0.30), (w * 0.46, h * 0.50)], fill=far)
    d.polygon([(w * 0.30, h * 0.50), (w * 0.62, h * 0.26), (w, h * 0.46), (w, h * 0.50)],
              fill=far)
    d.polygon([(0, h * 0.52), (w * 0.34, h * 0.40), (w * 0.70, h * 0.52)], fill=near)
    d.rectangle((0, h * 0.50, w, h * 0.52), fill=near)
    for i in range(9):                    # trees along the shore
        x = w * (0.05 + 0.11 * i)
        d.polygon([(x, h * 0.36), (x - w * 0.03, h * 0.50), (x + w * 0.03, h * 0.50)],
                  fill=(30, 72, 48))
    d.polygon([(w * 0.44, h * 0.74), (w * 0.58, h * 0.74), (w * 0.55, h * 0.80),
               (w * 0.47, h * 0.80)], fill=(196, 60, 52))
    d.rectangle((w * 0.509, h * 0.60, w * 0.517, h * 0.74), fill=(236, 236, 226))
    d.polygon([(w * 0.517, h * 0.60), (w * 0.517, h * 0.73), (w * 0.60, h * 0.73)],
              fill=(244, 244, 236))
    d.rectangle((w * 0.06, h * 0.055, w * 0.28, h * 0.115), fill=(248, 248, 242))
    d.text((w * 0.07, h * 0.07), "EK410 soak", fill=(22, 22, 22))
    return im


def _pad_to(dst: Path, size: int) -> None:
    """Bytes after EOI / IEND: the decoders stop at the marker, the length is
    what counts on the device."""
    have = dst.stat().st_size
    if size and have < size:
        with dst.open("ab") as f:
            f.write(b"\x00" * (size - have))


def _exif_picture(w: int, h: int):
    """Stored the way a phone stores a portrait shot: turned by 90° with the
    EXIF orientation tag that says so. A big red square sits top-left as a
    viewer must show it; where the panel puts it tells whether the tag was
    honoured. The square stays inside the strip that survives the cover crop
    in either orientation."""
    from PIL import Image, ImageDraw
    upright = _art(w, h, 0)
    m = 240
    ImageDraw.Draw(upright).rectangle((400, 60, 400 + m, 60 + m), fill=(188, 42, 40))
    rotate = getattr(Image, "Transpose", Image).ROTATE_90        # counter-clockwise
    exif = Image.Exif()
    exif[0x0112] = 6                                            # "rotate 90° CW to show"
    return upright.transpose(rotate), exif


def build_image_set(www: Path) -> None:
    """Generate the image set. Pillow is needed for this only."""
    try:
        from PIL import Image, ImageDraw
    except ImportError:
        sys.exit("Pillow is needed once to generate the image set "
                 "(use the ESP-IDF Python environment)")
    import frame_convert
    www.mkdir(parents=True, exist_ok=True)
    on_disk = [name for name, spec, _, _ in IMAGE_SET if spec[0] not in NOT_STORED]
    stamp = www / "set.json"
    spec_now = json.dumps(IMAGE_SET)
    if stamp.exists() and stamp.read_text() != spec_now:
        # the set was redefined since it was generated: start over
        for name in on_disk:
            (www / name).unlink(missing_ok=True)
    for name, spec, expect, reason in IMAGE_SET:
        dst = www / name
        if dst.exists() or spec[0] in SERVED:
            continue
        kind = spec[0]
        if kind == "jpeg":
            _, variant, w, h, quality, pad_to = spec
            _art(w, h, variant).save(dst, "JPEG", quality=quality)
            _pad_to(dst, pad_to)
        elif kind == "jpeggray":
            _, w, h = spec
            _art(w, h, 0).convert("L").save(dst, "JPEG", quality=90)
        elif kind == "png":
            _, mode, w, h, pad_to = spec
            im = _art(w, h, {"RGB": 2, "P": 1, "L": 0}[mode])
            if mode == "P":             # a six-colour push, as the web console sends
                im = im.convert("P", palette=Image.ADAPTIVE, colors=6)
            elif mode == "L":
                im = im.convert("L")
            im.save(dst, "PNG", optimize=True)
            _pad_to(dst, pad_to)
        elif kind == "alpha":
            _, w, h = spec
            canvas = Image.new("RGBA", (w, h), (0, 0, 0, 0))     # transparent round it
            canvas.paste(_art(w // 2, h // 2, 1).convert("RGBA"), (w // 4, h // 4))
            canvas.save(dst, "PNG", optimize=True)
        elif kind == "exif":
            _, w, h = spec
            stored, exif = _exif_picture(w, h)
            stored.save(dst, "JPEG", quality=90, exif=exif.tobytes())
        elif kind == "noise":
            _, w, h, quality = spec
            n = Image.effect_noise((w, h), 48).convert("L")
            Image.merge("RGB", (n, n.transpose(getattr(Image, "Transpose", Image).FLIP_LEFT_RIGHT),
                            n)).save(
                dst, "JPEG", quality=quality)
        elif kind == "pad":
            _, src, size = spec
            data = (www / src).read_bytes()
            if len(data) > size:
                sys.exit(f"{src} is {len(data)} bytes, cannot pad it to {size}")
            dst.write_bytes(data + b"\x00" * (size - len(data)))
        elif kind == "raw":
            frame_convert.convert(str(www / spec[1]), str(dst))
            if not dst.exists() or dst.stat().st_size != RAW_FRAME_BYTES:
                sys.exit(f"frame_convert did not produce a {RAW_FRAME_BYTES}-byte {dst}")
        elif kind == "truncate":
            _, src, n = spec
            dst.write_bytes((www / src).read_bytes()[:n])
        elif kind == "progressive":
            _, src = spec
            Image.open(www / src).save(dst, "JPEG", quality=85, progressive=True)
    stamp.write_text(spec_now)

    for name, spec, expect, reason in IMAGE_SET:
        if spec[0] in NOT_STORED or expect == "source":
            continue
        size = (www / name).stat().st_size
        over = size > IMAGE_LIMIT
        if over != (reason == OVER_LIMIT):
            sys.exit(f"{name} is {size} bytes, which does not match its "
                     f"expectation ({expect}" + (f", {reason})" if reason else ")"))
        if expect == "display" and spec[0] != "pad" and size > IMAGE_LIMIT - 64 * 1024:
            sys.exit(f"{name} is {size} bytes: a picture that has to display sits "
                     "well under the limit")
        log(f"  {name:18} {size:8d} bytes")


# ---------- local image servers ----------

class ImageHandler(http.server.SimpleHTTPRequestHandler):
    def log_message(self, fmt, *a):  # keep the soak's own output readable
        pass

    def handle(self):
        try:
            super().handle()
        except (BrokenPipeError, ConnectionResetError):
            pass    # the device closed early — it refuses a file on its headers

    def _drop(self):
        self.close_connection = True
        try:
            self.connection.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass

    def _behave(self, spec):
        """What a server can do wrong."""
        kind = spec[0]
        if kind == "status":
            self.send_response(spec[1])
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", "12")
            self.end_headers()
            self.wfile.write(b"no such file")
            return
        data = Path(self.translate_path(spec[1])).read_bytes()
        if kind == "chunked":
            self.protocol_version = "HTTP/1.1"
            self.send_response(200)
            self.send_header("Content-Type", "image/jpeg")
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            for off in range(0, len(data), 32768):
                chunk = data[off:off + 32768]
                self.wfile.write(f"{len(chunk):X}\r\n".encode() + chunk + b"\r\n")
            self.wfile.write(b"0\r\n\r\n")
            self.wfile.flush()
            self.close_connection = True
        elif kind == "nolength":
            self.protocol_version = "HTTP/1.0"           # the body ends at EOF
            self.send_response(200)
            self.send_header("Content-Type", "image/jpeg")
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(data)
            self.wfile.flush()
            self.close_connection = True
        else:
            sent = data[:int(len(data) * spec[2])] if kind == "cut" else data[:64 * 1024]
            self.send_response(200)
            self.send_header("Content-Type", "image/jpeg")
            self.send_header("Content-Length", str(len(data)))   # promises it all
            self.end_headers()
            self.wfile.write(sent)
            self.wfile.flush()
            if kind == "stall":
                time.sleep(STALL_S)
            self._drop()

    def do_GET(self):
        route = self.path.split("?")[0]
        if route in self.server.behaviours:
            try:
                return self._behave(self.server.behaviours[route])
            except (BrokenPipeError, ConnectionResetError):
                return
        target = self.server.redirects.get(route)
        if target is None:
            return super().do_GET()
        self.send_response(302)
        self.send_header("Location", target)
        self.send_header("Content-Length", "0")
        self.end_headers()


class ImageServer(http.server.ThreadingHTTPServer):
    allow_reuse_address = True
    daemon_threads = True

    def __init__(self, www: Path, port: int, behaviours: dict | None = None):
        self.redirects = {"/" + name: spec[1] for name, spec, _, _ in IMAGE_SET
                          if spec[0] == "redirect"}
        self.behaviours = behaviours or {}
        super().__init__(("0.0.0.0", port),
                         functools.partial(ImageHandler, directory=str(www)))

    @property
    def port(self) -> int:
        return self.server_address[1]


def listen(www: Path, first_port: int, behaviours: dict | None = None):
    for port in range(first_port, first_port + 10):
        try:
            server = ImageServer(www, port, behaviours)
        except OSError:
            continue
        threading.Thread(target=server.serve_forever, daemon=True).start()
        return server
    return None


def self_signed_server(www: Path, tls_dir: Path, first_port: int):
    """An HTTPS server with a certificate no device can accept: the TLS
    failure path, on a host this script controls. None without openssl."""
    openssl = shutil.which("openssl")
    if not openssl:
        return None
    tls_dir.mkdir(parents=True, exist_ok=True)
    cert, key = tls_dir / "cert.pem", tls_dir / "key.pem"
    if not cert.exists() or not key.exists():
        subprocess.run([openssl, "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                        "-keyout", str(key), "-out", str(cert), "-days", "30",
                        "-subj", "/CN=ek410-soak"], check=True, capture_output=True)
    server = None
    for port in range(first_port, first_port + 10):
        try:
            server = ImageServer(www, port)
        except OSError:
            continue
        break
    if server is None:
        return None
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(cert, key)
    server.socket = context.wrap_socket(server.socket, server_side=True)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server


def lan_ip_towards(host: str) -> str:
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect((host, 9))       # no packet is sent for UDP connect
        return s.getsockname()[0]
    finally:
        s.close()


DIRECT = urllib.request.build_opener(urllib.request.ProxyHandler({}))


def reachable_from_host(url: str) -> bool:
    """Fetched directly, the way the device fetches — no system proxy."""
    try:
        with DIRECT.open(url, timeout=20) as r:
            return r.status == 200
    except (urllib.error.URLError, OSError, ValueError):
        return False


# ---------- device (only these routes are ever called) ----------

class Device:
    def __init__(self, host: str):
        self.host = host

    def call(self, method: str, path: str, obj=None, body: bytes | None = None,
             timeout: float = 8.0):
        data = body if body is not None else (
            None if obj is None else json.dumps(obj).encode())
        req = urllib.request.Request(f"http://{self.host}{path}", data=data, method=method)
        if obj is not None:
            req.add_header("Content-Type", "application/json")
        try:
            with urllib.request.urlopen(req, timeout=timeout) as r:
                return r.status, r.read()
        except urllib.error.HTTPError as e:
            return e.code, e.read()
        except (urllib.error.URLError, OSError, http.client.HTTPException) as e:
            return None, str(e).encode()      # a cut answer (IncompleteRead) too

    def _json(self, path: str):
        status, data = self.call("GET", path)
        try:
            return json.loads(data.decode("utf-8", "replace")) if status == 200 else None
        except ValueError:
            return None

    def get(self, path: str):
        """GET, parsed."""
        return self._json(path)

    def info(self):
        return self._json("/api/v1/system/getDevInfo")

    def diag(self):
        return self._json("/api/v1/system/getDiag") or {}

    def wait_info(self, seconds: float):
        """First successful getDevInfo within `seconds`, else None."""
        end = time.time() + seconds
        while True:
            info = self.info()
            if isinstance(info, dict) and "uptime_s" in info:
                return info
            if time.time() >= end:
                return None
            time.sleep(2)

    def meta(self) -> dict:
        """What the device says about its saved frame: present, seq, crc32,
        savedUs (µs since boot), bytes, source."""
        return self._json("/api/v1/display/getLastFrameMeta") or {}

    def events(self) -> str:
        """The device's event log, one line per event (plain text)."""
        status, data = self.call("GET", "/api/v1/system/eventLog", timeout=20)
        return data.decode("utf-8", "replace") if status == 200 and isinstance(data, bytes) else ""

    def frame(self) -> bytes:
        """The frame the device saved after the last display (may be short)."""
        status, data = self.call("GET", "/api/v1/display/lastFrame", timeout=120)
        return data if status == 200 and isinstance(data, bytes) else b""


def upload_firmware(host: str, data: bytes):
    conn = http.client.HTTPConnection(host, timeout=900)
    try:
        conn.putrequest("POST", "/api/v1/system/uploadFirmware")
        conn.putheader("Content-Type", "application/octet-stream")
        conn.putheader("Content-Length", str(len(data)))
        conn.endheaders()
        for off in range(0, len(data), 32768):
            conn.send(data[off:off + 32768])
        resp = conn.getresponse()
        return resp.status, resp.read()
    except (OSError, http.client.HTTPException) as e:
        return None, str(e).encode()
    finally:
        conn.close()


def web_console(dev: Device):
    """The web console as a browser loads it: the page, the local files it
    refers to, and the requests its status screen makes. (all served, bytes)"""
    status, page = dev.call("GET", "/")
    if status != 200 or not page or len(page) < 1000:
        return False, len(page or b"")
    served, total = True, len(page)
    for ref in dict.fromkeys(ASSET_REF.findall(page.decode("utf-8", "replace"))):
        if ref.startswith(("http:", "https:", "data:", "//")):
            continue
        status, body = dev.call("GET", "/" + ref.lstrip("/"), timeout=30)
        served = served and status == 200 and bool(body)
        total += len(body or b"")
    for path in ("/api/v1/system/getPowerParam", "/api/v1/display/getLastFrameMeta",
                 "/api/v1/network/getWifiParam"):
        status, body = dev.call("GET", path)
        served = served and status == 200 and bool(body)
        total += len(body or b"")
    return served, total


# ---------- the broker's side ----------

def prov_kind(info_) -> str | None:
    """Is somebody provisioning this unit right now? Its own flags in getDevInfo."""
    if not isinstance(info_, dict):
        return None
    if info_.get("bleProvUp"):
        return "BLE"
    if info_.get("softapUp"):
        return "SoftAP"
    return None


# "… up=83 BLE CT-Prov stopped", "… up=120 WIFI SoftAP up ssid=…"
PROV_EVENT = re.compile(r" up=(\d+) (BLE|WIFI) ((?:CT-Prov|SoftAP) (?:up|stopped|closed))")


def mqtt_command(kind: str, target: str) -> tuple[str, dict]:
    """A console request as the cloud sends it (main/mqtt.c handle_command)."""
    if kind == "canvas":
        return "render_canvas", CANVAS
    if kind == "text":
        return "render_text", TEXT_SCREEN
    return "display_url", {"url": target}


class BrokerMirror:
    """A second client on the broker the unit talks to, with the credentials
    taken from the unit (tools/broker_secret.py — the API reports
    `hasPassword` only). Subscribed to the unit's uplink topic it sees every
    telemetry and command acknowledgement, and it publishes downlink commands
    the way the cloud does. The credentials do not leave this object."""

    def __init__(self, secret: Path, client_id: str, topic_up: str, topic_down: str):
        try:
            import paho.mqtt.client as mqtt
        except ImportError:
            sys.exit("paho-mqtt is needed with --broker-secret "
                     "(the ESP-IDF Python environment has it)")
        cred = json.loads(Path(secret).expanduser().read_text())
        missing = [k for k in ("host", "port", "username", "password") if not cred.get(k)]
        if missing:
            sys.exit(f"{secret}: lacks {missing}")
        self._lock = threading.Lock()
        self._telemetry: list = []        # (received, object), older first
        self._acks: list = []
        self.other = 0                    # anything else the unit published
        self.state = {"up": False, "lost": 0}
        self._c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,
                              client_id=f"{client_id}-soak", protocol=mqtt.MQTTv311)
        self._c.username_pw_set(cred["username"], cred["password"])
        self._c.on_message = self._on_message
        self._c.on_connect = lambda c, u, f, rc, p=None: self.state.update(
            up=int(getattr(rc, "value", rc)) == 0)
        self._c.on_disconnect = self._on_disconnect
        self._c.connect(cred["host"], int(cred["port"]), keepalive=30)
        self._c.loop_start()
        end = time.time() + 15
        while not self.state["up"] and time.time() < end:
            time.sleep(0.1)
        if not self.state["up"]:
            sys.exit(f"the broker did not take the mirror client "
                     f"({cred['host']}:{cred['port']})")
        self.host, self.up, self.down = cred["host"], topic_up, topic_down
        self._c.subscribe(topic_up, qos=1)

    def _on_disconnect(self, c, u, f, rc, p=None):
        if self.state["up"]:
            self.state["lost"] += 1
        self.state["up"] = False

    def _on_message(self, c, u, m):
        try:
            obj = json.loads(m.payload)
        except ValueError:
            obj = {"type": "not JSON", "bytes": len(m.payload)}
        with self._lock:
            kind = obj.get("type") if isinstance(obj, dict) else None
            if kind == "telemetry":
                self._telemetry.append((time.time(), obj))
            elif kind == "cmd_ack":
                self._acks.append((time.time(), obj))
            else:
                self.other += 1

    @property
    def connected(self) -> bool:
        return self.state["up"]

    def publish(self, cmd: str, params: dict | None = None) -> bool:
        """A downlink command the way the cloud sends it: {"cmd", "params"}."""
        body = {"cmd": cmd}
        if params is not None:
            body["params"] = params
        info = self._c.publish(self.down, json.dumps(body, ensure_ascii=False).encode(), qos=1)
        try:
            info.wait_for_publish(5)
        except (ValueError, RuntimeError, OSError):
            return False
        return bool(info.is_published())

    def drop_acks_before(self, t: float) -> None:
        with self._lock:
            self._acks = [a for a in self._acks if a[0] >= t]

    def wait_ack(self, cmd: str, statuses, seconds: float, not_before: float | None = None):
        """The first acknowledgement of `cmd` with one of `statuses`, received
        at or after `not_before`. (received, object) or None."""
        end = time.time() + seconds
        while True:
            with self._lock:
                for i, (ts, obj) in enumerate(self._acks):
                    if ((obj.get("last_cmd") or "").lower() == cmd
                            and obj.get("cmd_status") in statuses
                            and (not_before is None or ts >= not_before)):
                        del self._acks[:i + 1]
                        return ts, obj
            if time.time() >= end:
                return None
            time.sleep(0.5)

    def wait_telemetry(self, seconds: float, not_before: float | None = None):
        """The first telemetry received at or after `not_before`."""
        end = time.time() + seconds
        while True:
            with self._lock:
                for i, (ts, obj) in enumerate(self._telemetry):
                    if not_before is None or ts >= not_before:
                        del self._telemetry[:i + 1]
                        return ts, obj
            if time.time() >= end:
                return None
            time.sleep(0.5)

    def close(self) -> None:
        try:
            self._c.loop_stop()
            self._c.disconnect()
        except Exception:  # noqa: BLE001 - ending the run is what matters
            pass
# ---------- the saved frame ----------

def nibble_counts(frame: bytes) -> collections.Counter:
    """Ink code → number of panel pixels."""
    counts: collections.Counter = collections.Counter()
    for byte, n in collections.Counter(frame).items():
        counts[byte >> 4] += n
        counts[byte & 15] += n
    return counts


def ink_at(frame: bytes, lx: int, ly: int) -> int:
    """Logical landscape pixel (lx, ly) of a native frame: two IC halves of
    1600 lines × 300 bytes, lx = native line, ly = 1199 − native column."""
    nx = 1199 - ly
    csx, local = divmod(nx, 600)
    byte = frame[csx * 480000 + lx * 300 + (local >> 1)]
    return (byte >> 4) if local % 2 == 0 else byte & 15


def red_row(frame: bytes) -> int:
    """Panel row of the centre of the red ink, or −1 without red ink."""
    sx = sy = n = 0
    for ly in range(0, 1200, 8):
        for lx in range(0, 1600, 8):
            if ink_at(frame, lx, ly) == IN_RED:
                sx += lx
                sy += ly
                n += 1
    return sy // n if n else -1


def border_dark_share(frame: bytes) -> float:
    """Share of non-white pixels along the four edges of the panel."""
    dark = total = 0
    for lx in range(0, 1600, 4):
        for ly in (2, 1197):
            dark += ink_at(frame, lx, ly) != IN_WHITE
            total += 1
    for ly in range(0, 1200, 4):
        for lx in (2, 1597):
            dark += ink_at(frame, lx, ly) != IN_WHITE
            total += 1
    return round(dark / total, 4)


# ---------- serial ----------

class SerialWatch(threading.Thread):
    """Passive reader: faults on serial, and the firmware's own reason for a
    failed URL ("url frame fail: …"). Opened once at start and kept for the
    whole run — nothing else can hold the port meanwhile."""

    MAX_LOG = 64 * 1024 * 1024

    def __init__(self, port: str, out: Path):
        super().__init__(daemon=True)
        self.port, self.out = port, out
        self.faults: list[dict] = []
        self.defects: list[dict] = []      # every DEFECT_MARKER of the run
        self._defects_new: list[dict] = [] # …not yet reported by the round loop
        self.round_no = 0
        self.lost = False
        self.error: str | None = None
        self._reason: str | None = None
        self._lock = threading.Lock()
        self._opened = threading.Event()
        self._stop = threading.Event()

    def run(self):
        import serial
        path = self.out / "serial.log"
        try:
            ser = serial.Serial(self.port, 115200, timeout=1)
            f = path.open("ab")
        except Exception as e:  # noqa: BLE001 - reported through self.error
            self.error = str(e)
            self._opened.set()
            return
        f.write(f"\n==== soak serial log {time.strftime('%Y-%m-%d %H:%M:%S')} ====\n".encode())
        self._opened.set()
        while not self._stop.is_set():
            try:
                line = ser.readline()
            except Exception as e:  # noqa: BLE001
                log(f"serial: read failed ({e}) — continuing without it")
                self.lost = True
                break
            if not line:
                continue
            text = line.decode("utf-8", "replace").rstrip()
            f.write(text.encode() + b"\n")
            f.flush()
            if f.tell() > self.MAX_LOG:
                f.close()
                os.replace(path, self.out / "serial.log.1")
                f = path.open("ab")
            low = text.split(") ", 1)[-1]
            if "url frame fail: " in low:
                with self._lock:
                    self._reason = low.split("url frame fail: ", 1)[1].strip()
            elif any(m in text for m in FAULT_MARKERS):
                with self._lock:
                    self.faults.append({"ts": time.time(), "round": self.round_no,
                                        "text": low[:160]})
            elif any(m in text for m in DEFECT_MARKERS):
                with self._lock:
                    hit = {"ts": time.time(), "round": self.round_no, "text": low[:160]}
                    self.defects.append(hit)
                    self._defects_new.append(hit)
        f.close()

    def take_reason(self):
        """The failure reason logged since the previous call, then forgets it."""
        with self._lock:
            reason, self._reason = self._reason, None
            return reason

    def defect_count(self) -> int:
        with self._lock:
            return len(self.defects)

    def take_defects(self) -> list:
        """The defect markers logged since the previous call, then forgets them."""
        with self._lock:
            new, self._defects_new = self._defects_new, []
            return new

    def fault_count(self) -> int:
        with self._lock:
            return len(self.faults)

    def stop(self):
        self._stop.set()


# ---------- the soak ----------

def write_json(path: Path, obj) -> None:
    tmp = path.with_suffix(path.suffix + ".tmp")
    with tmp.open("w", encoding="utf-8") as f:
        json.dump(obj, f, indent=2, ensure_ascii=False)
        f.write("\n")
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)


def main() -> int:
    ap = argparse.ArgumentParser(description="EK410 long soak (business rhythm, one unit)")
    ap.add_argument("--host", required=True, help="the device IP — only this unit is touched")
    ap.add_argument("--out", default=str(Path.home() / "ek410-soak"),
                    help="directory for the image set, rounds.jsonl, serial.log, summary.json")
    ap.add_argument("--hours", type=float, default=72.0)
    ap.add_argument("--rounds", type=int, default=0, help="stop after N rounds instead (smoke test)")
    ap.add_argument("--interval", type=float, default=45.0,
                    help="seconds between displays; MQTT telemetry runs meanwhile")
    ap.add_argument("--port", default="", help="serial port to watch (opened once; must be free)")
    ap.add_argument("--serve-port", type=int, default=8000)
    ap.add_argument("--tls-port", type=int, default=8443)
    ap.add_argument("--server-ip", default="", help="IP the device uses to reach this computer")
    ap.add_argument("--upload-every", type=int, default=25,
                    help="every N rounds push the raw frame with uploadFrame instead")
    ap.add_argument("--reject-every", type=int, default=17,
                    help="every N rounds send a file the firmware must refuse")
    ap.add_argument("--fault-every", type=int, default=29,
                    help="every N rounds hit a server fault (404, cut, stall, bad certificate)")
    ap.add_argument("--content-every", type=int, default=6,
                    help="every N rounds show a dashboard canvas or a text screen")
    ap.add_argument("--burst-every", type=int, default=4,
                    help="every N picture rounds, other requests while it is displayed")
    ap.add_argument("--web-every", type=int, default=10,
                    help="every N rounds load the web console afterwards")
    ap.add_argument("--https-url", default="",
                    help="public HTTPS image; adds the TLS path to the run")
    ap.add_argument("--https-every", type=int, default=0,
                    help="every N rounds display --https-url instead (0: never)")
    ap.add_argument("--ota-every-hours", type=float, default=0.0,
                    help="every H hours upload --image again; the unit must come back in "
                         "the other slot (0: never)")
    ap.add_argument("--give-up-min", type=float, default=30.0,
                    help="exit when the device stays unreachable this long")
    ap.add_argument("--image", type=Path, default=None,
                    help="firmware file: recorded by sha256, and re-uploaded with "
                         "--ota-every-hours")
    ap.add_argument("--no-caffeinate", action="store_true")
    ap.add_argument("--broker-secret", type=Path, default=None,
                    help="file with the unit's broker credentials (tools/broker_secret.py): "
                         "adds the broker's side to the run — a telemetry is checked after "
                         "every round, and every --mqtt-every rounds the command goes over "
                         "the broker instead of HTTP")
    ap.add_argument("--mqtt-every", type=int, default=7,
                    help="every N rounds send the command over the broker")
    ap.add_argument("--ping-every", type=int, default=25,
                    help="every N rounds ping the unit over the broker between displays")
    args = ap.parse_args()

    if (platform.system() == "Darwin" and not args.no_caffeinate
            and os.environ.get("EK410_SOAK_CAFFEINATED") != "1"
            and shutil.which("caffeinate")):
        os.environ["EK410_SOAK_CAFFEINATED"] = "1"
        log("re-executing under caffeinate -i -s (the host will not idle-sleep)")
        os.execvp("caffeinate", ["caffeinate", "-i", "-s", sys.executable,
                                 os.path.abspath(__file__)] + sys.argv[1:])

    out = Path(args.out).expanduser()
    out.mkdir(parents=True, exist_ok=True)
    if (out / "STOP").exists():
        sys.exit(f"{out / 'STOP'} exists — remove it to start a new run")
    pid_path = out / "soak.pid"
    if pid_path.exists():
        try:
            old = int(pid_path.read_text().strip())
        except ValueError:
            old = 0
        if old != os.getpid() and old and _alive(old):
            sys.exit(f"a soak is already running on {out} (pid {old})")
    pid_path.write_text(f"{os.getpid()}\n")

    www = out / "www"
    log(f"image set → {www}")
    build_image_set(www)

    behaviours = {"/" + name: spec for name, spec, _ in FAULT_SET if spec[0] != "selfsigned"}
    behaviours.update({"/" + name: (spec[1], spec[2]) for name, spec, _, _ in IMAGE_SET
                       if spec[0] == "serve"})
    server = listen(www, args.serve_port, behaviours)
    if server is None:
        sys.exit("no free port for the image server")
    ip = args.server_ip or lan_ip_towards(args.host)
    base = f"http://{ip}:{server.port}"
    log(f"image server on {base}")
    faults = list(FAULT_SET)
    tls = None
    if any(spec[0] == "selfsigned" for _, spec, _ in faults):
        tls = self_signed_server(www, out / "tls", args.tls_port)
        if tls is None:
            faults = [f for f in faults if f[1][0] != "selfsigned"]
            log("no openssl / no free port: the self-signed certificate fault is left out")
        else:
            log(f"self-signed HTTPS server on https://{ip}:{tls.port}")

    watch = SerialWatch(args.port, out) if args.port else None
    if watch:
        watch.start()      # before anything else: the port must be free
        watch._opened.wait(30)
        if watch.error:
            server.shutdown()
            pid_path.unlink(missing_ok=True)
            sys.exit(f"serial: cannot open {watch.port}: {watch.error} "
                     "(close whatever holds it)")
        time.sleep(2)

    dev = Device(args.host)
    log(f"waiting for {args.host} to answer getDevInfo")
    first = dev.wait_info(180)
    if first is None:
        server.shutdown()
        pid_path.unlink(missing_ok=True)
        sys.exit("device did not answer getDevInfo within 180 s")
    # Right after a boot the web server answers before MQTT has connected
    # (~50 s): start once the unit is fully up, so every round checks MQTT.
    t = time.time()
    while not first.get("mqttConnected") and time.time() - t < 150:
        time.sleep(3)
        first = dev.wait_info(10)
        if first is None:
            break
    fw = first.get("fwVersion")
    log(f"device up: fw {fw}, uptime {first.get('uptime_s')} s, "
        f"MQTT connected: {first.get('mqttConnected')}")
    # minFreeInt is the lowest internal free heap since boot: a new low later
    # on can only be seen against the value the run starts with.
    boot_heap = dev.diag().get("heap") or {}
    low_water = {"internal": boot_heap.get("minFreeInt")}
    log(f"heap after boot: freeInt={boot_heap.get('freeInt')} "
        f"minFreeInt={boot_heap.get('minFreeInt')} largestInt={boot_heap.get('largestInt')} "
        f"spiram={boot_heap.get('freeSpiram')}/{boot_heap.get('largestSpiram')}")

    mirror, device_id = None, ""
    if args.broker_secret:
        mq = dev.get("/api/v1/network/getMqttParam") or {}
        if not (mq.get("topicUp") and mq.get("topicDown") and mq.get("clientId")):
            server.shutdown()
            pid_path.unlink(missing_ok=True)
            sys.exit("the unit does not report its MQTT topics (getMqttParam)")
        device_id = str(mq["clientId"]).split("-", 1)[-1]
        mirror = BrokerMirror(args.broker_secret, mq["clientId"], mq["topicUp"], mq["topicDown"])
        log(f"on the broker's side as well: {mirror.host}, subscribed to {mirror.up}")
    rounds_path, summary_path = out / "rounds.jsonl", out / "summary.json"
    (out / "frames").mkdir(exist_ok=True)
    image_data = args.image.read_bytes() if args.image and args.image.exists() else b""
    image_sha = hashlib.sha256(image_data).hexdigest()[:12] if image_data else None
    raw_bytes = (www / "raw.bin").read_bytes()
    raw_sha = hashlib.sha256(raw_bytes).hexdigest()[:12]
    clock = Clock()
    started, started_mono = time.time(), time.monotonic()
    stats = {"rounds": 0, "displayed": 0, "uploads": 0, "https": 0, "canvas": 0, "text": 0,
             "faults_expected": 0, "rejected_expected": 0, "rejected_unexpected": 0,
             "requests_refused": 0, "reboots": 0, "mqtt_not_restored": 0,
             "mqtt_down_rounds": 0, "mqtt_down_polls": 0, "unreachable": 0,
             "https_unreachable_here": 0, "host_sleep_rounds": 0,
             "burst_rounds": 0, "burst_anomalies": 0, "frame_checks": 0, "frame_problems": 0,
             "planned_ota": 0, "ota_failed": 0,
             "mqtt_rounds": 0, "mqtt_ack_missing": 0, "mqtt_ack_wrong": 0,
             "telemetry_checks": 0, "telemetry_problems": 0, "mqtt_silent": 0,
             "pings": 0, "ping_failed": 0, "outages": 0, "mqtt_down_gaps": 0,
             "prov_sessions": 0, "prov_exit_failures": 0}
    unexpected: list[dict] = []
    incidents: list[dict] = []
    heap_series: list[dict] = []
    stack_min: dict[str, int] = {}
    frame_ref: dict[str, str] = {}     # content → the first frame it produced
    frame_changes: collections.Counter = collections.Counter()
    last_saved_digest = [None]         # …of the frame on flash: an identical one is not written again
    prov_pending = [None]              # provisioning seen on the unit since the last round
    info: dict[str, object] = {}       # what the frames show (not judged)
    last_uptime = first.get("uptime_s") if isinstance(first.get("uptime_s"), int) else None
    round_no = rejects = displays = fault_i = content_i = burst_i = ota_done = 0
    ended = "target reached"
    summary: dict = {}

    def summarise(finished: bool) -> dict:
        def col(key):
            return finite(h.get(key) for h in heap_series)

        def ends(key):
            vals = col(key) or [None]
            return {"first": vals[0], "last": vals[-1], "min": min(vals)}
        return {
            "finished": finished, "ended": ended,
            "host": args.host, "fw": fw, "image": image_sha,
            "started": time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(started)),
            "updated": time.strftime("%Y-%m-%d %H:%M:%S"),
            "elapsed_h": round((time.monotonic() - started_mono) / 3600, 3),
            "target_h": args.hours,
            "host_slept_h": round(clock.frozen / 3600, 3),
            **stats,
            "serial": ("off" if not watch else
                       "lost" if watch.lost or watch.error else "watched"),
            "serial_faults": watch.fault_count() if watch else 0,
            "serial_defects": watch.defect_count() if watch else 0,
            "ok": (stats["reboots"] == 0 and stats["rejected_unexpected"] == 0
                   and stats["requests_refused"] == 0 and stats["mqtt_not_restored"] == 0
                   and stats["unreachable"] == 0 and stats["burst_anomalies"] == 0
                   and stats["frame_problems"] == 0 and stats["ota_failed"] == 0
                   and stats["mqtt_ack_missing"] == 0 and stats["mqtt_ack_wrong"] == 0
                   and stats["telemetry_problems"] == 0 and stats["mqtt_silent"] == 0
                   and stats["ping_failed"] == 0 and stats["outages"] == 0
                   and stats["mqtt_down_gaps"] == 0 and stats["prov_exit_failures"] == 0
                   and (not watch or (watch.fault_count() == 0
                                      and watch.defect_count() == 0))),
            "unexpected": unexpected[-20:],
            "incidents": incidents[-50:],
            "frames": {key: {"first": digest, "changed": frame_changes[key]}
                       for key, digest in sorted(frame_ref.items())},
            "info": info,
            "heap": {"freeInt": ends("freeInt"),
                     "minFreeInt": {"min": min(col("minFreeInt"), default=None)},
                     "largestInt": {"min": min(col("largestInt"), default=None)},
                     "freeSpiram": ends("freeSpiram"),
                     "largestSpiram": {"min": min(col("largestSpiram"), default=None)}},
            "stack_min": dict(sorted(stack_min.items(), key=lambda kv: kv[1])),
            "internal_low_water": low_water["internal"],
            "broker": mirror.host if mirror else None,
            "mirror_reconnects": mirror.state["lost"] if mirror else 0,
            "criteria": "ok = no unexpected reboot, every display had the intended "
                        "result, no request refused, MQTT back after every display, "
                        "device always reachable, no misbehaviour under concurrent "
                        "requests, every checked frame as sent and never blank, every "
                        "planned OTA completed, no fault or defect marker on serial; "
                        "with --broker-secret: the broker told how each command ended, "
                        "a truthful telemetry after every round, pings answered, MQTT never "
                        "down for a whole gap, and the unit a client again after any "
                        "provisioning on it",
        }

    def check_frame(rec: dict, key: str, must_equal: str | None = None,
                    blank_check: bool = False, judged_change: bool = False):
        """Read the frame the device saved. Hard problems: not a frame, not
        what was sent (upload, raw), blank (canvas, text) — and for a canvas
        or text, different from the first one of that content, since nothing
        in their rendering depends on timing or memory. A picture that comes
        out differently is only counted. (problems, frame, saved this round)"""
        frame = b""
        for _ in range(5):       # telemetry reads the flash copy now and then
            frame = dev.frame()
            if len(frame) == RAW_FRAME_BYTES:
                break
            time.sleep(1)
        if len(frame) != RAW_FRAME_BYTES:
            return [f"the saved frame is {len(frame)} bytes, not {RAW_FRAME_BYTES}"], frame, False
        digest = hashlib.sha256(frame).hexdigest()[:12]
        counts = nibble_counts(frame)
        rec["frame"] = digest
        rec["inks"] = {f"{code:x}": round(n / (2 * RAW_FRAME_BYTES), 4)
                       for code, n in sorted(counts.items())}
        problems = []
        if key not in frame_ref:
            frame_ref[key] = digest
            (out / "frames" / f"{key.replace('/', '_')}.bin").write_bytes(frame)
        elif frame_ref[key] != digest:
            frame_changes[key] += 1
            if judged_change:
                problems.append(f"frame {digest} differs from the first one ({frame_ref[key]})")
        if must_equal and digest != must_equal:
            problems.append(f"frame {digest} is not what was sent ({must_equal})")
        if blank_check and counts[IN_WHITE] > 0.99 * 2 * RAW_FRAME_BYTES:
            problems.append("the screen came out blank")
        meta = dev.meta()
        saved = meta.get("savedUs")
        rec["frame_source"] = meta.get("source")
        rec["frame_crc"] = zlib.crc32(frame) & 0xFFFFFFFF   # as the telemetry reports it
        up_before = rec.get("up_before")
        fresh = (isinstance(saved, (int, float)) and saved > 0
                 and isinstance(up_before, int) and saved / 1e6 >= up_before)
        return problems, frame, fresh

    def burst(pattern: int):
        """Other requests while the panel is refreshing, as when somebody
        uses the console during a display. The firmware answers status
        requests, queues a second display behind the one running, lets a
        proxyImage wait for it, and lets an uploadFrame wait for the panel
        lock — then paints the upload and drops what was queued.
        (what happened, what must not have, what the saved frame should hold)"""
        res: dict = {"pattern": BURST_PATTERNS[pattern % len(BURST_PATTERNS)]}
        bad: list[str] = []
        shown = None
        polls = set()
        for _ in range(5):
            polls.add(dev.call("GET", "/api/v1/system/getDevInfo", timeout=10)[0])
            time.sleep(0.4)
        if None in polls and dev.wait_info(900) is not None and last_prov_event():
            # the web server went away under us: provisioning was opened on
            # the unit (somebody pressed a button) — the round is about that now
            return {"pattern": res["pattern"], "provisioning": last_prov_event()}, [], None
        res["poll"] = sorted(str(s) for s in polls)
        bad += [f"getDevInfo answered {s} during a refresh" for s in polls if s != 200]
        if res["pattern"] == "poll":
            served, nbytes = web_console(dev)
            res["console"] = f"{'served' if served else 'NOT served'}, {nbytes} bytes"
            if not served:
                bad.append("the web console was not served during a refresh")
        elif res["pattern"] == "proxy":
            t = time.time()
            # errors come back as HTTP 200 with a code, like every refusal here
            status, data = dev.call("POST", "/api/v1/display/proxyImage",
                                    {"url": f"{base}/jpeg.jpg"}, timeout=200)
            res["proxy"] = f"{status}, {len(data or b'')} bytes in {time.time() - t:.0f}s"
            if _code(data) is not None:
                res["proxy"] += f" — {data[:60]!r}"
            if status != 200 or _code(data) is not None or len(data or b"") < 1000:
                bad.append(f"proxyImage during a refresh: {res['proxy']}")
        elif res["pattern"] == "second":
            status, resp = dev.call("POST", "/api/v1/display/displayUrl",
                                    {"url": f"{base}/icon.png"})
            if status == 200 and _code(resp) == 0:
                res["second"] = "queued"          # shown right after the picture
                shown = "icon.png"
            else:
                res["second"] = f"not queued: {status} {resp[:60]!r}"
            if status is None:
                bad.append("a second displayUrl got no answer")
        else:
            # 1.3.11 refuses it at once ("panel busy") and the server throws
            # the body away. Until 1.3.10 it waited for the panel with the
            # body unread — and lost Wi-Fi every time on the bench.
            t = time.time()
            status, data = dev.call("POST", "/api/v1/display/uploadFrame", body=raw_bytes,
                                    timeout=UPLOAD_TIMEOUT_S)
            secs = time.time() - t
            refused = status == 200 and _code(data) == 1 and b"busy" in data
            res["upload"] = (f"refused ({data[:40]!r}) in {secs:.0f} s" if refused
                            else f"{status} in {secs:.0f} s — {data[:60]!r}")
            if not refused or secs > 20:
                bad.append(f"uploadFrame behind a refresh: {res['upload']}")
        return res, bad, shown

    def wait_mqtt(seconds: float):
        """First getDevInfo with MQTT connected within `seconds`, else None."""
        end = time.time() + seconds
        while True:
            d = dev.wait_info(5)
            if d is not None and d.get("mqttConnected"):
                return d
            if time.time() >= end:
                return None
            time.sleep(2)

    def broker_ping() -> bool:
        """ping on the broker, and its acknowledgement."""
        stats["pings"] += 1
        t = time.time()
        mirror.drop_acks_before(t)
        if not mirror.publish("ping"):
            stats["ping_failed"] += 1
            return False
        ack = mirror.wait_ack("ping", ("ok", "error"), 15, t)
        if not (ack and ack[1].get("cmd_status") == "ok"):
            stats["ping_failed"] += 1
            return False
        return True

    def telemetry_check(not_before: float, rec: dict | None) -> list[str]:
        """A telemetry from `not_before` on that tells the broker what the unit
        just showed. What is wrong with it — nothing, when all is right.
        (the first one takes up to 55 s: the period is 30 s)"""
        got = mirror.wait_telemetry(TELEMETRY_S + 25, not_before)
        if got is None:
            return ["no telemetry on the broker for 55 s with MQTT up"]
        obj = got[1]
        bad = []
        if obj.get("deviceId") not in (None, device_id):
            bad.append(f"telemetry is from device {obj.get('deviceId')!r}")
        if fw and obj.get("fwVersion") != fw:
            bad.append(f"telemetry says firmware {obj.get('fwVersion')!r}")
        now_up = (dev.info() or {}).get("uptime_s")
        up = obj.get("uptime_s")
        if isinstance(up, int) and isinstance(now_up, int) and not 0 <= now_up - up <= 150:
            bad.append(f"telemetry uptime {up} s against the unit's {now_up} s")
        if obj.get("ip") and obj["ip"] != args.host:
            bad.append(f"telemetry says ip {obj['ip']}, the unit is {args.host}")
        # the preview the broker holds: there whenever a frame was saved, and
        # of the frame just shown (1.3.10 lost it after every failed fetch)
        try:
            bmp = base64.b64decode(obj.get("frame_preview_b64") or "", validate=True)
        except (ValueError, TypeError):
            bmp = b""
        w, h = obj.get("frame_preview_w"), obj.get("frame_preview_h")
        if obj.get("frame_present"):
            if len(bmp) < PREVIEW_BMP or bmp[:2] != b"BM":
                bad.append(f"telemetry has a frame but no usable preview ({len(bmp)} bytes)")
            elif (int.from_bytes(bmp[18:22], "little", signed=True),
                  abs(int.from_bytes(bmp[22:26], "little", signed=True))) != (w, h):
                bad.append(f"telemetry preview is not {w}x{h} as it says")
        if rec is not None and "frame_crc" in rec and obj.get("frame_crc32") != rec["frame_crc"]:
            bad.append(f"telemetry carries frame crc {obj.get('frame_crc32')}, "
                       f"the saved frame is {rec['frame_crc']}")
        return bad

    def last_prov_event() -> str | None:
        """Provisioning the unit's event log shows in the last 15 minutes."""
        now_up = (dev.wait_info(10) or {}).get("uptime_s")
        for line in reversed(dev.events().splitlines()):
            m = PROV_EVENT.search(line)
            if m:
                recent = isinstance(now_up, int) and now_up - int(m.group(1)) <= 900
                return ("BLE" if m.group(2) == "BLE" else "SoftAP") if recent else None
        return None

    def prov_exit_check(rec: dict | None, how: str) -> bool:
        """Somebody opened provisioning on the unit — its PAGE / MAIN button,
        the console, the app. Once it is over the unit has to be a client
        again (1.3.10 never was after BLE: largest internal block 4–8 KB
        against the 8 KB MQTT needs). False when provisioning never ends."""
        stats["prov_sessions"] += 1
        t0, kind = time.time(), how
        while True:
            d = dev.wait_info(10)
            pk = prov_kind(d)
            if d is not None and pk is None:
                break
            kind = pk or kind
            if time.time() - t0 > 1800 or (out / "STOP").exists():
                return False
            # getDevInfo counts as traffic on the hotspot: ask less often
            # than its two-minute idle timeout, or it would never close
            time.sleep(130 if kind == "SoftAP" else 3)
        why = [] if wait_mqtt(PROV_S) else ["MQTT is down after it"]
        if not why and mirror:
            if not broker_ping():
                why.append("no ping acknowledgement on the broker")
            else:
                lost = mirror.state["lost"]
                bad = telemetry_check(time.time() - 5, None)
                why += bad or (["the mirror client lost the broker meanwhile"]
                               if mirror.state["lost"] != lost else [])
        entry = {"ts": time.time(), "round": round_no, "what": f"{kind} provisioning on the unit",
                 "secs": round(time.time() - t0), "back_as_client": not why,
                 "why": "; ".join(why)}
        incidents.append(entry)
        if rec is not None:
            rec["provisioning"] = entry
        if why:
            stats["prov_exit_failures"] += 1
            unexpected.append({**(rec or {"round": round_no}),
                               "why": f"after {kind} provisioning: {'; '.join(why)}"})
            log(f"*** {kind} provisioning ended after {entry['secs']} s — NOT back as a "
                f"client: {'; '.join(why)} ***")
        else:
            log(f"{kind} provisioning ended after {entry['secs']} s — back as a client")
        return True

    try:
        with rounds_path.open("a", encoding="utf-8", buffering=1) as rounds:
            def emit(rec: dict) -> None:
                rounds.write(json.dumps(rec, ensure_ascii=False, default=str) + "\n")
                rounds.flush()
                os.fsync(rounds.fileno())

            def serial_report(round_no: int):
                """New fault and defect markers on serial → incidents."""
                if not watch:
                    return
                seen = len([i for i in incidents if i.get("what") == "serial fault"])
                for fault in watch.faults[seen:]:
                    incidents.append({**fault, "what": "serial fault"})
                    log(f"*** serial fault: {fault['text']} ***")
                for defect in watch.take_defects():
                    incidents.append({**defect, "what": "serial defect"})
                    log(f"*** serial defect: {defect['text']} ***")

            emit({"event": "session", "ts": started, "fw": fw, "image": image_sha,
                  "host": args.host})
            while True:
                if (out / "STOP").exists():
                    log("STOP file found — ending the run")
                    ended = "STOP file"
                    break
                if args.rounds and round_no >= args.rounds:
                    ended = "rounds"
                    break
                if not args.rounds and (time.monotonic() - started_mono) >= args.hours * 3600:
                    break
                slept = clock.check()
                if slept:
                    incidents.append({"ts": time.time(), "round": round_no,
                                      "what": "host slept", "secs": round(slept)})
                    log(f"host was frozen for {slept / 60:.0f} min before this round")
                round_no += 1
                stats["rounds"] = round_no
                if watch:
                    watch.round_no = round_no
                frozen_before = clock.frozen
                elapsed_h = (time.monotonic() - started_mono) / 3600

                # ---- a planned OTA is a round of its own: a reboot is the point
                if args.ota_every_hours and image_data and \
                        elapsed_h >= (ota_done + 1) * args.ota_every_hours:
                    ota_done += 1
                    t0 = time.time()
                    rec = {"round": round_no, "ts": t0, "kind": "ota",
                           "name": "uploadFirmware (same image)", "expect": "other slot"}
                    slot = (dev.info() or {}).get("otaPartition")
                    status, body = upload_firmware(dev.host, image_data)
                    problems = []
                    if not (status == 200 and _code(body) == 0):
                        problems.append(f"upload refused: status={status} {body[:80]!r}")
                    after, end = None, time.time() + 300
                    while not problems and time.time() < end:
                        after = dev.wait_info(10)
                        if after and after.get("otaPartition") != slot:
                            break
                        after = None
                        time.sleep(3)
                    if not problems and after is None:
                        problems.append("did not come back in the other slot within 300 s")
                    if after is not None and not problems:
                        end = time.time() + 120
                        while after.get("otaPendingVerify") and time.time() < end:
                            time.sleep(3)
                            after = dev.wait_info(10) or after
                        if after.get("otaPendingVerify"):
                            problems.append("stayed pending-verify")
                        if after.get("fwVersion") != fw:
                            problems.append(f"came back as {after.get('fwVersion')!r}, was {fw!r}")
                    if after is not None and not problems:
                        # let MQTT come up, as at the start of the run
                        end = time.time() + 150
                        while not after.get("mqttConnected") and time.time() < end:
                            time.sleep(3)
                            after = dev.wait_info(10) or after
                    last_uptime = None
                    rec["secs"] = round(time.time() - t0, 1)
                    if problems:
                        stats["ota_failed"] += 1
                        unexpected.append({**rec, "why": "; ".join(problems)})
                        log(f"*** planned OTA FAILED: {'; '.join(problems)} ***")
                    else:
                        stats["planned_ota"] += 1
                        log(f"[r{round_no} {elapsed_h:5.2f}h] planned OTA: {slot} -> "
                            f"{after.get('otaPartition')}, confirmed, {rec['secs']:.0f}s")
                    rec["verdict"] = "ok" if not problems else "OTA FAILED"
                    emit(rec)
                    serial_report(round_no)
                    write_json(summary_path, summarise(False))
                    continue

                # ---- what this round shows
                kind, name, url = "url", "", ""
                expect, want = "display", None
                plain = False
                if args.upload_every and round_no % args.upload_every == 0:
                    kind, name = "upload", "raw.bin (uploadFrame)"
                elif args.https_url and args.https_every and round_no % args.https_every == 0:
                    kind, url = "https", args.https_url
                elif faults and args.fault_every and round_no % args.fault_every == 0:
                    fname, fspec, want = faults[fault_i % len(faults)]
                    fault_i += 1
                    kind, name, expect = "fault", fname, "reject"
                    url = (f"https://{ip}:{tls.port}{fspec[1]}" if fspec[0] == "selfsigned"
                           else f"{base}/{fname}")
                elif args.reject_every and round_no % args.reject_every == 0:
                    entry = TO_REJECT[rejects % len(TO_REJECT)]
                    rejects += 1
                    kind, name, expect, want = "reject", entry[0], entry[2], entry[3]
                elif args.content_every and round_no % args.content_every == 0:
                    kind, name = (("canvas", "dashboard canvas") if content_i % 2 == 0
                                  else ("text", "text screen"))
                    content_i += 1
                else:
                    name = TO_DISPLAY[displays % len(TO_DISPLAY)][0]
                    displays += 1
                    plain = True
                t0 = time.time()
                rec = {"round": round_no, "ts": t0, "kind": kind, "name": name or url,
                       "expect": expect}
                if url:
                    rec["url"] = url

                before = dev.wait_info(60)
                if before is None:
                    # the web server is down while BLE provisioning is up
                    log(f"device unreachable — waiting up to {args.give_up_min:.0f} min")
                    before = dev.wait_info(args.give_up_min * 60)
                    if before is None:
                        stats["unreachable"] += 1
                        incidents.append({"ts": time.time(), "round": round_no,
                                          "what": "gave up"})
                        ended = "gave up: device unreachable"
                        break
                    pk = last_prov_event()
                    if pk:
                        prov_pending[0] = pk
                    else:
                        stats["unreachable"] += 1
                        incidents.append({"ts": time.time(), "round": round_no,
                                          "what": "unreachable before round"})
                if prov_pending[0] or prov_kind(before):
                    # somebody is provisioning the unit: wait it out, then see
                    # whether it is a client again
                    how = prov_kind(before) or prov_pending[0]
                    log(f"{how} provisioning is up on the unit — waiting")
                    note = {"round": round_no, "ts": time.time(), "kind": "provisioning",
                            "name": f"{how} provisioning"}
                    if not prov_exit_check(note, how):
                        ended = ("STOP file" if (out / "STOP").exists()
                                 else "provisioning never ended")
                        break
                    note["verdict"] = "not judged — provisioning on the unit"
                    emit(note)
                    prov_pending[0] = None
                    before = dev.wait_info(30)
                    if before is None:
                        continue
                    t0 = time.time()          # the display starts here, not at the wait
                up_before = before.get("uptime_s")
                mqtt_was_up = bool(before.get("mqttConnected"))
                if not mqtt_was_up:
                    stats["mqtt_down_rounds"] += 1
                rebooted = False
                if isinstance(up_before, int) and isinstance(last_uptime, int) \
                        and up_before < last_uptime:
                    rebooted = True
                    incidents.append({"ts": time.time(), "round": round_no,
                                      "what": "reboot between rounds",
                                      "uptime": [last_uptime, up_before]})
                rec.update(up_before=up_before, mqtt_before=mqtt_was_up)

                via = ("mqtt" if mirror and mirror.connected and args.mqtt_every
                       and kind != "upload" and round_no % args.mqtt_every == 0 else "http")
                rec["via"] = via
                cmd = None
                tel_wait = 0.0
                if via == "mqtt":
                    # the cloud's way in: a command on the broker, answered there
                    cmd, params = mqtt_command(kind, url or f"{base}/{name}")
                    t_cmd = time.time()
                    mirror.drop_acks_before(t_cmd)
                    stats["mqtt_rounds"] += 1
                    if not mirror.publish(cmd, params):
                        status, data = None, b"publish failed"
                    else:
                        ack = mirror.wait_ack(cmd, ("queued", "error", "ok"), ACK_S, t_cmd)
                        if ack is None:
                            status, data = None, b"no acknowledgement on the broker"
                        else:
                            rec["ack"] = {k: ack[1].get(k) for k in ("cmd_status", "detail")}
                            taken = ack[1].get("cmd_status") in ("queued", "ok")
                            status, data = (200, b"\x00") if taken else (503, b"refused")
                elif kind == "upload":
                    status, data = dev.call("POST", "/api/v1/display/uploadFrame",
                                            body=raw_bytes, timeout=UPLOAD_TIMEOUT_S)
                elif kind == "canvas":
                    status, data = dev.call("POST", "/api/v1/display/renderCanvas", CANVAS)
                elif kind == "text":
                    status, data = dev.call("POST", "/api/v1/display/renderText", TEXT_SCREEN)
                else:
                    status, data = dev.call("POST", "/api/v1/display/displayUrl",
                                            {"url": url or f"{base}/{name}"})
                accepted = status == 200 and _code(data) in (0, None)
                rec["status"] = status
                if not accepted:
                    stats["requests_refused"] += 1
                    unexpected.append({**rec, "why": f"request not accepted "
                                                      f"({(data or b'')[:60]!r})"})
                    log(f"*** request not accepted: {rec['name']} status={status} "
                        f"{(data or b'')[:60]!r} ***")

                shown_as = None          # a burst that puts something else on the panel
                if plain and args.burst_every and round_no % args.burst_every == 0 \
                        and accepted:
                    # a few seconds in, the picture is being fetched and decoded
                    time.sleep(10)
                    rec["burst"], bad, shown_as = burst(burst_i)
                    burst_i += 1
                    if rec["burst"].get("provisioning"):
                        rec["prov_during"] = rec["burst"]["provisioning"]
                    stats["burst_rounds"] += 1
                    if bad:
                        stats["burst_anomalies"] += len(bad)
                        for what in bad:
                            incidents.append({"ts": time.time(), "round": round_no,
                                              "what": f"under load: {what}"})
                        log(f"*** under load: {'; '.join(bad)} ***")

                unreachable = 0
                while True:      # the display, or the replay after a reject
                    info_ = dev.wait_info(10)
                    clock.check()
                    if info_ is None:
                        unreachable += 1
                        if unreachable > 30:
                            break
                        continue
                    rec["up_during"] = info_.get("uptime_s")
                    if prov_kind(info_):
                        rec["prov_during"] = prov_kind(info_)
                        break          # the panel is not this run's to watch now
                    if not info_.get("displayBusy"):
                        break
                    if time.time() - t0 > DISPLAY_TIMEOUT_S:
                        rec["timeout"] = True
                        break
                    time.sleep(3)
                rec["secs"] = round(time.time() - t0, 1)
                rec["unreachable_polls"] = unreachable
                if rec.get("prov_during") or (unreachable and last_prov_event()):
                    # a button on the unit: BLE or the hotspot came up while
                    # this round ran. Not a verdict on the round — whether the
                    # unit is a client again afterwards is.
                    if not prov_exit_check(rec, rec.get("prov_during") or last_prov_event()) \
                            and (out / "STOP").exists():
                        rec["verdict"] = "not judged — provisioning opened during the round"
                        emit(rec)
                        ended = "STOP file"
                        break
                elif unreachable:
                    stats["outages"] += 1
                    incidents.append({"ts": time.time(), "round": round_no,
                                      "what": f"unreachable for {unreachable} polls during "
                                              f"the round"})
                    log(f"*** device unreachable for {unreachable} polls during the round ***")

                after = dev.wait_info(15)
                if after is not None:
                    up_after = after.get("uptime_s")
                    if isinstance(up_after, int) and isinstance(up_before, int) \
                            and up_after < up_before and not rebooted:
                        rebooted = True
                        incidents.append({"ts": time.time(), "round": round_no,
                                          "what": "reboot during round",
                                          "uptime": [up_before, up_after]})
                    if isinstance(up_after, int):
                        last_uptime = up_after
                    rec["displayed"] = bool(after.get("lastDisplayOk"))
                    rec["mqtt_restored"] = None
                    if mqtt_was_up:
                        # displayUrl stops MQTT for the HTTPS heap: it has to
                        # come back by itself.
                        t1 = time.time()
                        while not after.get("mqttConnected") and time.time() - t1 < MQTT_RESUME_S:
                            time.sleep(2)
                            after = dev.wait_info(10)
                            clock.check()
                            if after is None:
                                break
                        rec["mqtt_restored"] = bool(after and after.get("mqttConnected"))
                        if not rec["mqtt_restored"]:
                            stats["mqtt_not_restored"] += 1
                            rec["mqtt_reason"] = (after or {}).get("mqttReason", "unreachable")
                    diag = dev.diag()
                    rec["heap"] = diag.get("heap") or {}
                    rec["stack_min"] = diag.get("stackMin") or {}
                    mark = rec["heap"].get("minFreeInt")
                    if isinstance(mark, int) and isinstance(low_water["internal"], int) \
                            and mark < low_water["internal"]:
                        # the lowest point internal RAM has reached so far,
                        # and the step that took it there
                        incidents.append({"ts": time.time(), "round": round_no,
                                          "what": "internal RAM low-water",
                                          "from": low_water["internal"], "to": mark,
                                          "after": rec["name"]})
                        log(f"internal RAM low-water {low_water['internal']} → {mark} bytes "
                            f"after {rec['name']}")
                        low_water["internal"] = mark
                if rebooted:
                    stats["reboots"] += 1
                    rec["reboot"] = True
                    log("*** REBOOT detected ***")
                if watch:
                    rec["url_reason"] = watch.take_reason()
                    if (rec["url_reason"] is None and want and not rec.get("displayed")
                            and not watch.lost):
                        # the log line can trail displayBusy by a moment
                        end = time.time() + 5
                        while rec["url_reason"] is None and time.time() < end:
                            time.sleep(0.5)
                            rec["url_reason"] = watch.take_reason()
                    if watch.lost and not any(i.get("what") == "serial lost" for i in incidents):
                        incidents.append({"ts": time.time(), "round": round_no,
                                          "what": "serial lost"})
                        log("*** serial port lost — failure reasons are no longer checked ***")
                have_serial = bool(watch) and not watch.lost

                # A timed-out display or a reboot in the round is never a pass:
                # lastDisplayOk would only be the previous result.
                reasons = want if isinstance(want, list) else [want]
                intended = not rebooted and not rec.get("timeout") and (
                    (rec.get("displayed") is True and expect == "display") or (
                     rec.get("displayed") is False and expect == "reject"
                     and (not have_serial or not want or rec.get("url_reason") in reasons)))

                # ---- the frame the device saved
                frame_problems: list[str] = []
                if intended and expect == "display" and not rec.get("provisioning"):
                    stats["frame_checks"] += 1
                    uploaded = shown_as == "upload"
                    key = FRAME_KEYS.get(kind, name)   # the content the frame should hold
                    if name in RAW_FRAMES or shown_as:
                        key = shown_as or name
                    # what was sent must come back byte for byte
                    must_equal = raw_sha if (kind == "upload" or uploaded
                                             or (name in RAW_FRAMES and not shown_as)) else None
                    strict = kind in ("canvas", "text") and not shown_as
                    frame_problems, frame, fresh = check_frame(
                        rec, key, must_equal, blank_check=strict, judged_change=strict)
                    if fresh:
                        last_saved_digest[0] = rec.get("frame")
                    elif (not must_equal and not frame_problems
                          and rec.get("frame") != last_saved_digest[0]):
                        # the only other way: the frame on flash is already this
                        # one (two files that carry the same picture)
                        frame_problems.append("no frame was saved for this display")
                    if not shown_as and frame:
                        if name == "exif.jpg":
                            # "upright" = the tag was honoured; "as stored" = ignored
                            state = "upright" if 0 <= red_row(frame) < 600 else "as stored"
                            rec["exif"] = state
                            info.setdefault("exif_orientation_tag", [])
                            if state not in info["exif_orientation_tag"]:
                                info["exif_orientation_tag"].append(state)
                            if state != "upright":
                                # until 1.3.10 a phone's portrait shot came out sideways
                                frame_problems.append("the EXIF orientation was not applied")
                        elif name == "alpha.png":
                            rec["border_dark"] = border_dark_share(frame)
                            seen = set(info.get("alpha_border_dark") or [])
                            seen.add(rec["border_dark"])
                            info["alpha_border_dark"] = sorted(seen)
                if frame_problems:
                    stats["frame_problems"] += 1
                    rec["frame_problems"] = frame_problems

                # ---- what the broker was told
                cloud_problems: list[str] = []
                if cmd:
                    ack = mirror.wait_ack(cmd, ("ok", "error"), 60, t_cmd)
                    rec["ack_done"] = ({k: ack[1].get(k) for k in ("cmd_status", "detail")}
                                       if ack else None)
                    if ack is None:
                        stats["mqtt_ack_missing"] += 1
                        cloud_problems.append("the broker was not told how the command ended")
                    elif expect == "display" and ack[1].get("cmd_status") != "ok":
                        stats["mqtt_ack_wrong"] += 1
                        cloud_problems.append(f"the broker was told {ack[1].get('cmd_status')} "
                                              f"{ack[1].get('detail')!r}")
                    elif expect == "reject" and want and not (
                            ack[1].get("cmd_status") == "error"
                            and ack[1].get("detail") in reasons):
                        stats["mqtt_ack_wrong"] += 1
                        cloud_problems.append(f"the broker was told {ack[1].get('cmd_status')} "
                                              f"{str(ack[1].get('detail'))[:60]!r}, expected "
                                              f"one of {reasons}")

                # ---- what the broker is told about the unit
                if mirror and rec.get("mqtt_restored") is True and not rec.get("provisioning"):
                    lost = mirror.state["lost"]
                    t_tel = time.time()
                    tel_bad = telemetry_check(t_tel, rec if "frame_crc" in rec else None)
                    tel_wait = time.time() - t_tel
                    stats["telemetry_checks"] += 1
                    if mirror.state["lost"] != lost:
                        rec["telemetry"] = "not judged — this client lost the broker"
                        tel_bad = []
                    elif tel_bad:
                        rec["telemetry"] = tel_bad
                        stats["mqtt_silent" if tel_bad[0].startswith("no telemetry")
                              else "telemetry_problems"] += 1
                        cloud_problems += [f"telemetry: {b}" for b in tel_bad]
                    else:
                        rec["telemetry"] = "ok"

                if rec.get("provisioning"):
                    verdict = "not judged — provisioning opened during the round"
                elif intended and not frame_problems and not cloud_problems:
                    if expect == "reject":
                        stats["rejected_expected" if kind == "reject" else "faults_expected"] += 1
                        verdict = ("rejected as intended" if kind == "reject"
                                   else "server fault handled")
                    elif kind == "upload":
                        stats["uploads"] += 1
                        verdict = "uploaded"
                    elif kind == "https":
                        stats["https"] += 1
                        verdict = "displayed (https)"
                    elif kind in ("canvas", "text"):
                        stats[kind] += 1
                        verdict = f"{kind} displayed"
                    else:
                        stats["displayed"] += 1
                        verdict = "ok" + (" (via broker)" if via == "mqtt" else "")
                elif expect == "display" and kind == "https" and \
                        not reachable_from_host(url):
                    stats["https_unreachable_here"] += 1
                    verdict = "NOT DISPLAYED — this computer cannot fetch the URL either"
                elif clock.frozen > frozen_before:
                    stats["host_sleep_rounds"] += 1
                    verdict = "not judged — the host slept during the round"
                elif intended and not frame_problems and cloud_problems:
                    verdict = "BROKER PROBLEM"
                    unexpected.append({**rec, "why": "; ".join(cloud_problems)})
                else:
                    stats["rejected_unexpected"] += 1
                    verdict = "FRAME PROBLEM" if intended else "UNEXPECTED"
                    unexpected.append({**rec, "why": "; ".join(frame_problems) or verdict})
                rec["verdict"] = verdict
                heap_series.append({k: rec.get("heap", {}).get(k) for k in
                                    ("free", "minFree", "freeInt", "minFreeInt",
                                     "largestInt", "freeSpiram", "largestSpiram")})
                for task, val in (rec.get("stack_min") or {}).items():
                    if isinstance(val, int):
                        stack_min[task] = min(stack_min.get(task, val), val)
                emit(rec)
                serial_report(round_no)

                if args.web_every and round_no % args.web_every == 0:
                    served, nbytes = web_console(dev)
                    emit({"round": round_no, "event": "web_console", "ok": served,
                          "bytes": nbytes})
                    if not served:
                        stats["requests_refused"] += 1
                        unexpected.append({"round": round_no,
                                          "why": "web console not served"})
                        log("*** web console not served ***")

                h = rec.get("heap") or {}
                mqtt = {True: "ok", False: "DOWN", None: "was down"}[rec.get("mqtt_restored")]
                extra = ""
                if "burst" in rec:
                    extra = " burst=" + rec["burst"]["pattern"] + "".join(
                        f" [{k}: {rec['burst'][k]}]" for k in BURST_DETAIL if k in rec["burst"])
                if via == "mqtt":
                    extra += " via broker"
                if rec.get("ack"):
                    extra += f" ack={rec['ack']['cmd_status']}"
                if "ack_done" in rec:
                    extra += f" → {rec['ack_done'] and rec['ack_done']['cmd_status']}"
                if "telemetry" in rec:
                    extra += f" telemetry={rec['telemetry']}"
                if "exif" in rec:
                    extra += f" exif={rec['exif']}"
                if "border_dark" in rec:
                    extra += f" border_dark={rec['border_dark']}"
                log(f"[r{round_no} {(time.monotonic() - started_mono) / 3600:5.2f}h] "
                    f"{rec['name'][:30]:30} {verdict:22} {rec['secs']:5.1f}s mqtt={mqtt} "
                    f"int={h.get('freeInt')} largest={h.get('largestInt')} "
                    f"spiram={h.get('freeSpiram')}/{h.get('largestSpiram')} "
                    f"reason={rec.get('url_reason') or '-'}{extra}")
                summary = summarise(False)
                write_json(summary_path, summary)

                # Let telemetry (30 s period) run between rounds, and watch MQTT.
                tend = time.monotonic() + max(args.interval - tel_wait, 5.0)
                gap = {"round": round_no, "event": "gap", "mqtt_down_polls": 0,
                       "unreachable_polls": 0, "prov": None, "ping": None}
                reachable = 0
                while time.monotonic() < tend and not (out / "STOP").exists():
                    time.sleep(5)
                    clock.check()
                    cur = dev.info()
                    if cur is None:
                        gap["unreachable_polls"] += 1
                        continue
                    reachable += 1
                    if not cur.get("mqttConnected"):
                        gap["mqtt_down_polls"] += 1
                    if prov_kind(cur):
                        gap["prov"] = prov_kind(cur)
                        prov_pending[0] = prov_kind(cur)
                if gap["mqtt_down_polls"] and gap["mqtt_down_polls"] == reachable:
                    # reachable the whole time, MQTT down at every poll: the
                    # unit is not a client, and nothing is about to change it
                    stats["mqtt_down_gaps"] += 1
                    incidents.append({"ts": time.time(), "round": round_no,
                                      "what": "MQTT down between rounds"})
                    log("*** MQTT down between rounds ***")
                if mirror and args.ping_every and round_no % args.ping_every == 0 \
                        and not prov_pending[0]:
                    cur = dev.info()
                    if isinstance(cur, dict) and not cur.get("displayBusy"):
                        lost = mirror.state["lost"]
                        gap["ping"] = broker_ping()
                        if not gap["ping"] and mirror.state["lost"] == lost:
                            incidents.append({"ts": time.time(), "round": round_no,
                                              "what": "ping not acknowledged on the broker"})
                            log("*** ping on the broker not answered ***")
                stats["mqtt_down_polls"] += gap["mqtt_down_polls"]
                emit(gap)
                write_json(summary_path, summarise(False))
    except KeyboardInterrupt:
        log("interrupted — writing the summary")
        ended = "interrupted"
    finally:
        if watch:
            watch.stop()
        if mirror:
            mirror.close()
        server.shutdown()
        summary = summarise(True)
        write_json(summary_path, summary)
        pid_path.unlink(missing_ok=True)

    log(f"done ({ended}): {stats['rounds']} rounds — displayed {stats['displayed']}, "
        f"canvas {stats['canvas']}, text {stats['text']}, uploaded {stats['uploads']}, "
        f"https {stats['https']}, server faults handled {stats['faults_expected']}, "
        f"rejected as intended {stats['rejected_expected']}, UNEXPECTED "
        f"{stats['rejected_unexpected']}, under load {stats['burst_anomalies']}, frame problems "
        f"{stats['frame_problems']}, planned OTA {stats['planned_ota']} (failed "
        f"{stats['ota_failed']}), reboots {stats['reboots']}, MQTT not restored "
        f"{stats['mqtt_not_restored']}, over the broker {stats['mqtt_rounds']} rounds "
        f"(ack missing {stats['mqtt_ack_missing']}, wrong {stats['mqtt_ack_wrong']}), "
        f"telemetry checked {stats['telemetry_checks']} (problems {stats['telemetry_problems']}, "
        f"silent {stats['mqtt_silent']}), pings {stats['pings']} (failed {stats['ping_failed']}), "
        f"provisioning sessions {stats['prov_sessions']} (not back as a client "
        f"{stats['prov_exit_failures']}), serial faults {summary['serial_faults']}, serial defects "
        f"{summary['serial_defects']} → {'OK' if summary['ok'] else 'NOT OK'}")
    log(f"results in {out}")
    return 0 if summary["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
