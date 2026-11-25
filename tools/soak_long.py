#!/usr/bin/env python3
"""EK410 long soak — business rhythm on one unit, unattended.

Cycles URL displays served from this computer while MQTT telemetry keeps
running, samples heap and task stacks after every round, watches the serial
log for faults, and appends one JSON line per round — so the run can be read
at any time.

It never changes the device's configuration: the only requests are
displayUrl, uploadFrame, getDevInfo, getDiag and the web console.
`qa_gate.py --tier soak` is the stress rig (SoftAP cycles, broker flips,
wrong Wi-Fi password); this is the business-rhythm counterpart.

    python3 tools/soak_long.py --host 192.168.31.236 --out ~/ek410-soak \\
        --port /dev/cu.wchusbserialXXXX --hours 72 --image dist/ek410.bin

Round by round: a display from the image set; every --upload-every rounds a
raw frame through uploadFrame instead; every --reject-every rounds a file the
firmware must refuse; every --web-every rounds the web console is loaded
afterwards; with --https-url, every --https-every rounds an image from the
Internet, the only TLS path in the run. With --port the serial log is kept as
well: it is the only way to tell an intended reject from any other failure,
because the firmware logs its reason there ("url frame fail: …").

Needs Pillow once, to generate the image set into <out>/www (the ESP-IDF
Python environment has it). On macOS the script re-executes itself under
`caffeinate -i -s` so the host does not idle-sleep (`--no-caffeinate` turns
that off); a closed lid still sleeps a laptop — the run then goes on, reports
how long the host was away and does not judge the rounds it slept through.
Create <out>/STOP to end a run early; `summary.json` is rewritten after every
round. Exit code 0 only when the run met every criterion.
"""

from __future__ import annotations

import argparse
import functools
import hashlib
import http.server
import json
import os
import platform
import re
import shutil
import socket
import sys
import threading
import time
import urllib.error
import urllib.request
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))

IMAGE_LIMIT = 700 * 1024      # main/app_epd.c IMG_URL_IMAGE_MAX_BYTES
RAW_FRAME_BYTES = 960000
OVER_LIMIT = "image >700KB limit"
DISPLAY_TIMEOUT_S = 240       # a rejected URL replays the last frame (~50 s)
UPLOAD_TIMEOUT_S = 240        # uploadFrame returns after the refresh
MQTT_RESUME_S = 75
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
)
ASSET_REF = re.compile(r'(?:src|href)="([^"#]+)"')

# name, how it is built, what the device is expected to do, and for an
# intended reject the reason the firmware logs ("url frame fail: …").
# The pictures are smooth — the panel stands on a bench where people look at
# it. What matters to the device is the size of the body, so the big files are
# padded after EOI / IEND to the sizes that squeeze its memory: the decoders
# stop at the end marker, the body still takes the room. limit.jpg sits
# exactly on the limit.
IMAGE_SET = [
    ("jpeg.jpg", ("jpeg", 0, 1600, 1200, 90, 0), "display", None),
    ("photo.jpg", ("jpeg", 1, 1600, 1200, 92, 620 * 1024), "display", None),
    ("large.jpg", ("jpeg", 2, 2400, 1800, 90, 540 * 1024), "display", None),
    ("chart.png", ("png", "RGB", 2400, 1800, 0), "display", None),
    ("six.png", ("png", "P", 1600, 1200, 420 * 1024), "display", None),
    ("gray.png", ("png", "L", 800, 600, 150 * 1024), "display", None),
    ("limit.jpg", ("pad", "photo.jpg", IMAGE_LIMIT), "display", None),
    ("raw.bin", ("raw", "jpeg.jpg"), "display", None),
    ("redir.jpg", ("redirect", "/jpeg.jpg"), "display", None),
    ("redir-raw.bin", ("redirect", "/raw.bin"), "display", None),
    ("oversize.jpg", ("pad", "photo.jpg", IMAGE_LIMIT + 1024), "reject",
     OVER_LIMIT),
    ("trunc.png", ("truncate", "gray.png", 3000), "reject", "PNG decode fail"),
    ("progressive.jpg", ("progressive", "photo.jpg"), "reject", "JPEG decode fail"),
]
TO_DISPLAY = [entry for entry in IMAGE_SET if entry[2] == "display"]
TO_REJECT = [entry for entry in IMAGE_SET if entry[2] == "reject"]


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


def build_image_set(www: Path) -> None:
    """Generate the image set. Pillow is needed for this only."""
    try:
        from PIL import Image
    except ImportError:
        sys.exit("Pillow is needed once to generate the image set "
                 "(use the ESP-IDF Python environment)")
    import frame_convert
    www.mkdir(parents=True, exist_ok=True)
    on_disk = [name for name, spec, _, _ in IMAGE_SET if spec[0] != "redirect"]
    stamp = www / "set.json"
    spec_now = json.dumps(IMAGE_SET)
    if stamp.exists() and stamp.read_text() != spec_now:
        # the set was redefined since it was generated: start over
        for name in on_disk:
            (www / name).unlink(missing_ok=True)
    for name, spec, expect, reason in IMAGE_SET:
        dst = www / name
        if dst.exists():
            continue
        kind = spec[0]
        if kind == "jpeg":
            _, variant, w, h, quality, pad_to = spec
            _art(w, h, variant).save(dst, "JPEG", quality=quality)
            _pad_to(dst, pad_to)
        elif kind == "png":
            _, mode, w, h, pad_to = spec
            im = _art(w, h, {"RGB": 2, "P": 1, "L": 0}[mode])
            if mode == "P":             # a six-colour push, as the web console sends
                im = im.convert("P", palette=Image.ADAPTIVE, colors=6)
            elif mode == "L":
                im = im.convert("L")
            im.save(dst, "PNG", optimize=True)
            _pad_to(dst, pad_to)
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
        # "redirect": served from the table below, nothing on disk
    stamp.write_text(spec_now)

    for name, spec, expect, reason in IMAGE_SET:
        if spec[0] in ("redirect", "raw"):
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


# ---------- local image server ----------

class ImageHandler(http.server.SimpleHTTPRequestHandler):
    def log_message(self, fmt, *a):  # keep the soak's own output readable
        pass

    def handle(self):
        try:
            super().handle()
        except (BrokenPipeError, ConnectionResetError):
            pass    # the device closed early — it refuses a file on its headers

    def do_GET(self):
        target = self.server.redirects.get(self.path.split("?")[0])
        if target is None:
            return super().do_GET()
        self.send_response(302)
        self.send_header("Location", target)
        self.send_header("Content-Length", "0")
        self.end_headers()


class ImageServer(http.server.ThreadingHTTPServer):
    allow_reuse_address = True
    daemon_threads = True

    def __init__(self, www: Path, port: int):
        self.redirects = {"/" + name: spec[1] for name, spec, _, _ in IMAGE_SET
                          if spec[0] == "redirect"}
        super().__init__(("0.0.0.0", port),
                         functools.partial(ImageHandler, directory=str(www)))

    @property
    def port(self) -> int:
        return self.server_address[1]


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
        except (urllib.error.URLError, OSError) as e:
            return None, str(e).encode()

    def _json(self, path: str):
        status, data = self.call("GET", path)
        try:
            return json.loads(data.decode("utf-8", "replace")) if status == 200 else None
        except ValueError:
            return None

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


def web_console(dev: Device):
    """The web console as a browser loads it: the page and the local files it
    refers to. (everything was served, bytes transferred)"""
    status, page = dev.call("GET", "/")
    if status != 200 or not page or len(page) < 1000:
        return False, len(page or b"")
    served, total = True, len(page)
    refs = ASSET_REF.findall(page.decode("utf-8", "replace"))
    for ref in dict.fromkeys(refs):
        if ref.startswith(("http:", "https:", "data:", "//")):
            continue
        status, body = dev.call("GET", "/" + ref.lstrip("/"), timeout=30)
        served = served and status == 200 and bool(body)
        total += len(body or b"")
    return served, total


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
    ap.add_argument("--server-ip", default="", help="IP the device uses to reach this computer")
    ap.add_argument("--upload-every", type=int, default=25,
                    help="every N rounds push the raw frame with uploadFrame instead")
    ap.add_argument("--reject-every", type=int, default=17,
                    help="every N rounds send a file the firmware must refuse")
    ap.add_argument("--web-every", type=int, default=10,
                    help="every N rounds load the web console afterwards")
    ap.add_argument("--https-url", default="",
                    help="public HTTPS image; adds the TLS path to the run")
    ap.add_argument("--https-every", type=int, default=0,
                    help="every N rounds display --https-url instead (0: never)")
    ap.add_argument("--give-up-min", type=float, default=30.0,
                    help="exit when the device stays unreachable this long")
    ap.add_argument("--image", type=Path, default=None, help="firmware file, to record its sha256")
    ap.add_argument("--no-caffeinate", action="store_true")
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

    server = None
    for port in range(args.serve_port, args.serve_port + 10):
        try:
            server = ImageServer(www, port)
            break
        except OSError:
            continue
    if server is None:
        sys.exit("no free port for the image server")
    threading.Thread(target=server.serve_forever, daemon=True).start()
    base = f"http://{args.server_ip or lan_ip_towards(args.host)}:{server.port}"
    log(f"image server on {base}")

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

    rounds_path, summary_path = out / "rounds.jsonl", out / "summary.json"
    image_sha = (hashlib.sha256(args.image.read_bytes()).hexdigest()[:12]
                 if args.image and args.image.exists() else None)
    clock = Clock()
    started, started_mono = time.time(), time.monotonic()
    stats = {"rounds": 0, "displayed": 0, "uploads": 0, "https": 0,
             "rejected_expected": 0, "rejected_unexpected": 0,
             "requests_refused": 0, "reboots": 0, "mqtt_not_restored": 0,
             "mqtt_down_rounds": 0, "mqtt_down_polls": 0, "unreachable": 0,
             "https_unreachable_here": 0, "host_sleep_rounds": 0}
    unexpected: list[dict] = []
    incidents: list[dict] = []
    heap_series: list[dict] = []
    stack_min: dict[str, int] = {}
    last_uptime = first.get("uptime_s") if isinstance(first.get("uptime_s"), int) else None
    round_no, rejects, displays = 0, 0, 0
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
                   and stats["unreachable"] == 0
                   and (not watch or (watch.fault_count() == 0
                                      and watch.defect_count() == 0))),
            "unexpected": unexpected[-20:],
            "incidents": incidents[-50:],
            "heap": {"freeInt": ends("freeInt"),
                     "minFreeInt": {"min": min(col("minFreeInt"), default=None)},
                     "largestInt": {"min": min(col("largestInt"), default=None)},
                     "freeSpiram": ends("freeSpiram"),
                     "largestSpiram": {"min": min(col("largestSpiram"), default=None)}},
            "stack_min": dict(sorted(stack_min.items(), key=lambda kv: kv[1])),
            "internal_low_water": low_water["internal"],
            "criteria": "ok = no unexpected reboot, every display had the intended "
                        "result, no request refused, MQTT back after every display, "
                        "device always reachable, no fault or defect marker on serial",
        }

    try:
        with rounds_path.open("a", encoding="utf-8", buffering=1) as rounds:
            def emit(rec: dict) -> None:
                rounds.write(json.dumps(rec, ensure_ascii=False, default=str) + "\n")
                rounds.flush()
                os.fsync(rounds.fileno())

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

                kind, name, url = "url", "", ""
                expect, want_reason = "display", None
                if args.upload_every and round_no % args.upload_every == 0:
                    kind, name = "upload", "raw.bin (uploadFrame)"
                elif args.https_url and args.https_every and round_no % args.https_every == 0:
                    kind, url = "https", args.https_url
                elif args.reject_every and round_no % args.reject_every == 0:
                    entry = TO_REJECT[rejects % len(TO_REJECT)]
                    rejects += 1
                    name, expect, want_reason = entry[0], entry[2], entry[3]
                else:
                    name = TO_DISPLAY[displays % len(TO_DISPLAY)][0]
                    displays += 1
                t0 = time.time()
                rec = {"round": round_no, "ts": t0, "kind": kind, "name": name or url,
                       "expect": expect}

                before = dev.wait_info(60)
                if before is None:
                    stats["unreachable"] += 1
                    incidents.append({"ts": time.time(), "round": round_no,
                                      "what": "unreachable before round"})
                    log(f"device unreachable — waiting up to {args.give_up_min:.0f} min")
                    before = dev.wait_info(args.give_up_min * 60)
                    if before is None:
                        incidents.append({"ts": time.time(), "round": round_no,
                                          "what": "gave up"})
                        ended = "gave up: device unreachable"
                        break
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

                if kind == "upload":
                    status, data = dev.call("POST", "/api/v1/display/uploadFrame",
                                            body=(www / "raw.bin").read_bytes(),
                                            timeout=UPLOAD_TIMEOUT_S)
                    accepted = status == 200 and _code(data) == 0
                else:
                    target = url or f"{base}/{name}"
                    rec["url"] = target
                    status, resp = dev.call("POST", "/api/v1/display/displayUrl",
                                            {"url": target})
                    accepted = status == 200 and _code(resp) in (0, None)
                rec["status"] = status
                if not accepted:
                    stats["requests_refused"] += 1
                    unexpected.append({**rec, "why": "request not accepted"})
                    log(f"*** request not accepted: {rec['name']} status={status} ***")

                unreachable = 0
                while True:      # the display, or the replay after a reject
                    info = dev.wait_info(10)
                    clock.check()
                    if info is None:
                        unreachable += 1
                        if unreachable > 24:
                            break
                        continue
                    rec["up_during"] = info.get("uptime_s")
                    if not info.get("displayBusy"):
                        break
                    if time.time() - t0 > DISPLAY_TIMEOUT_S:
                        rec["timeout"] = True
                        break
                    time.sleep(3)
                rec["secs"] = round(time.time() - t0, 1)
                rec["unreachable_polls"] = unreachable

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
                    if watch.lost and not any(i.get("what") == "serial lost" for i in incidents):
                        incidents.append({"ts": time.time(), "round": round_no,
                                          "what": "serial lost"})
                        log("*** serial port lost — reject reasons are no longer checked ***")
                have_serial = bool(watch) and not watch.lost

                # A timed-out display or a reboot in the round is never a pass:
                # lastDisplayOk would only be the previous result.
                intended = not rebooted and not rec.get("timeout") and (
                    (rec.get("displayed") is True and expect == "display") or (
                     rec.get("displayed") is False and expect == "reject"
                     and (not have_serial or not want_reason
                          or rec.get("url_reason") == want_reason)))
                if intended:
                    if expect == "reject":
                        stats["rejected_expected"] += 1
                        verdict = "rejected as intended"
                    elif kind == "upload":
                        stats["uploads"] += 1
                        verdict = "uploaded"
                    elif kind == "https":
                        stats["https"] += 1
                        verdict = "displayed (https)"
                    else:
                        stats["displayed"] += 1
                        verdict = "ok"
                elif expect == "display" and kind == "https" and \
                        not reachable_from_host(url):
                    stats["https_unreachable_here"] += 1
                    verdict = "NOT DISPLAYED — this computer cannot fetch the URL either"
                elif clock.frozen > frozen_before:
                    stats["host_sleep_rounds"] += 1
                    verdict = "not judged — the host slept during the round"
                else:
                    stats["rejected_unexpected"] += 1
                    verdict = "UNEXPECTED"
                    unexpected.append(rec)
                rec["verdict"] = verdict
                heap_series.append({k: rec.get("heap", {}).get(k) for k in
                                    ("free", "minFree", "freeInt", "minFreeInt",
                                     "largestInt", "freeSpiram", "largestSpiram")})
                for task, val in (rec.get("stack_min") or {}).items():
                    if isinstance(val, int):
                        stack_min[task] = min(stack_min.get(task, val), val)
                emit(rec)

                if watch and watch.fault_count():
                    seen = len([i for i in incidents if i.get("what") == "serial fault"])
                    for fault in watch.faults[seen:]:
                        incidents.append({**fault, "what": "serial fault"})
                        log(f"*** serial fault: {fault['text']} ***")
                    for defect in watch.take_defects():
                        incidents.append({**defect, "what": "serial defect"})
                        log(f"*** serial defect: {defect['text']} ***")

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
                log(f"[r{round_no} {(time.monotonic() - started_mono) / 3600:5.2f}h] "
                    f"{rec['name'][:34]:34} {verdict:22} {rec['secs']:5.1f}s mqtt={mqtt} "
                    f"int={h.get('freeInt')} largest={h.get('largestInt')} "
                    f"spiram={h.get('freeSpiram')}/{h.get('largestSpiram')} "
                    f"reason={rec.get('url_reason') or '-'}")
                summary = summarise(False)
                write_json(summary_path, summary)

                # Let telemetry (30 s period) run between rounds, and watch MQTT.
                tend = time.monotonic() + args.interval
                gap = {"round": round_no, "event": "gap", "mqtt_down_polls": 0,
                       "unreachable_polls": 0}
                while time.monotonic() < tend and not (out / "STOP").exists():
                    time.sleep(5)
                    clock.check()
                    info = dev.info()
                    if info is None:
                        gap["unreachable_polls"] += 1
                    elif not info.get("mqttConnected"):
                        gap["mqtt_down_polls"] += 1
                stats["mqtt_down_polls"] += gap["mqtt_down_polls"]
                emit(gap)
                write_json(summary_path, summarise(False))
    except KeyboardInterrupt:
        log("interrupted — writing the summary")
        ended = "interrupted"
    finally:
        if watch:
            watch.stop()
        server.shutdown()
        summary = summarise(True)
        write_json(summary_path, summary)
        pid_path.unlink(missing_ok=True)

    log(f"done ({ended}): {stats['rounds']} rounds — displayed {stats['displayed']}, "
        f"uploaded {stats['uploads']}, https {stats['https']}, rejected as intended "
        f"{stats['rejected_expected']}, UNEXPECTED {stats['rejected_unexpected']}, reboots "
        f"{stats['reboots']}, MQTT not restored {stats['mqtt_not_restored']}, serial faults "
        f"{summary['serial_faults']} → {'OK' if summary['ok'] else 'NOT OK'}")
    log(f"results in {out}")
    return 0 if summary["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
