#!/usr/bin/env python3
"""tools/soak_long.py against a simulated unit and a simulated broker.

The soak has crashed on the bench more often on its own bugs than it has
caught the firmware's. This runs the whole tool — image set, servers, round
loop, verdicts — against a unit and a broker that answer from a table, with a
clock that only moves when the tool sleeps, so every kind of round (pictures,
refusals, server faults, canvas, text, upload, https, the four bursts, a
command over the broker, a ping, a planned OTA, a provisioning session) takes
a second. Then the same with something broken, and the verdict has to find it.

    RESULT: pass — N rounds, every kind of round, M injected faults all found
"""
import base64
import json
import os
import shutil
import sys
import tempfile
import time as real_time
from pathlib import Path
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
try:
    import PIL.Image  # noqa: F401
except ImportError:
    print("RESULT: skip — Pillow is needed to generate the image set")
    raise SystemExit(0)

import soak_long  # noqa: E402

W = soak_long.RAW_FRAME_BYTES
HOST, FW = "192.0.2.10", "0.0.0-test"       # TEST-NET-1: never a real unit
FAULT_REASON = {"http-404.jpg": "HTTP status 404", "cut.jpg": "http read error",
                "stall.jpg": "http read error", "selfsigned.jpg": "http open/TLS fail"}
REJECT_REASON = {e[0]: e[3] for e in soak_long.IMAGE_SET if e[2] == "reject"}
NIBBLES = (0, 2, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15)   # not white, not red


def frame_for(text):
    """A frame of one ink, a different one per content."""
    h = zlib.crc32(text.encode())
    return bytes([(NIBBLES[h % 14] << 4) | NIBBLES[(h >> 8) % 14]]) * W


class FakeClock:
    """Moves only when the tool sleeps, and equally for both clocks."""

    def __init__(self):
        self.t = 1_800_000_000.0

    def time(self):
        return self.t

    def monotonic(self):
        return self.t

    def sleep(self, seconds):
        self.t += seconds

    strftime = staticmethod(real_time.strftime)
    localtime = staticmethod(real_time.localtime)


class FakeDevice(soak_long.Device):
    """What the unit answers, as far as the soak asks. flags: what is broken."""
    flags: dict = {}
    raw = b""
    dev = None

    def __init__(self, host):
        super().__init__(host)
        self.up, self.seq = 500, 10
        self.busy_until, self.pending, self.queued = 0.0, None, None
        self.ok, self.bitmap, self.source = True, frame_for("boot"), "url"
        self.saved, self.saved_up, self.mqtt, self.slot = frame_for("boot"), 500, True, "ota_0"
        self.starts, self.dark, self.prov = 0, 0, 0
        self.prov_line, self.after_upload = "", None
        FakeDevice.dev = self

    def _job(self, name, target=None, cmd=None):
        """(shown?, frame, source, reason, command over the broker)."""
        if name in FAULT_REASON:
            return False, None, None, FAULT_REASON[name], cmd
        if name in REJECT_REASON:
            return False, None, None, REJECT_REASON[name], cmd
        if name in ("raw.bin", "redir-raw.bin"):
            return True, self.raw, "url", None, cmd
        if name == "canvas":
            key = "canvas" + str(self.starts) if self.flags.get("canvas_drifts") else "canvas"
            return True, frame_for(key), "canvas", None, cmd
        if name == "text":
            return True, frame_for("text"), "text", None, cmd
        if name == "exif.jpg":               # as the unit draws it with its tag honoured:
            return True, b"\x11" * (W // 2) + b"\x33" * (W // 2), "url", None, cmd
        return True, frame_for("jpeg.jpg" if name in ("chunked.jpg", "nolength.jpg")
                               else (target or name)), "url", None, cmd

    @staticmethod
    def name_of(url):
        """What a displayUrl asks for: the file, or the self-signed server."""
        return "selfsigned.jpg" if url.startswith("https://127.0.0.1:") else url.rsplit("/", 1)[-1]

    def start(self, name, target=None, cmd=None):
        self.starts += 1
        if self.starts == self.flags.get("prov_after"):
            self.prov = 1
        job = self._job(name, target, cmd)
        if self.busy_until:
            if self.after_upload is None:      # the upload would drop it anyway
                self.queued = job
        else:
            self._begin(job)

    def _begin(self, job):
        """On the panel for 50 s, or 40 s for the replay after a refusal."""
        self.pending, self.busy_until = job, soak_long.time.t + (50 if job[0] else 40)

    def _commit(self, frame, source):
        self.ok, self.bitmap, self.source = True, frame, source
        if frame == self.saved or self.flags.get("no_save"):
            return                     # unchanged: no erase, no write, no new save
        self.saved, self.saved_up, self.seq = frame, self.up, self.seq + 1

    def _tick(self):
        self.up += 1
        if self.prov == 1:                      # seen with the flag up, then the web server goes
            self.prov, self.dark = 2, 3
            self.busy_until, self.pending, self.queued = 0.0, None, None
            return True
        if self.prov == 2:                      # back from provisioning
            self.prov = 0
            self.mqtt = not self.flags.get("prov_breaks_mqtt")
            self.prov_line = f" up={self.up} BLE CT-Prov stopped"
        if self.busy_until and soak_long.time.t >= self.busy_until:
            self.busy_until = 0.0
            if True:
                if self.pending:
                    if self.pending[0]:
                        self._commit(self.pending[1], self.pending[2])
                    else:
                        self.ok = False
                    if self.pending[4]:
                        broker = FakeBroker.current
                        if broker and not broker.flags.get("swallow_done"):
                            broker.acks.append({"last_cmd": self.pending[4],
                                                "cmd_status": "ok" if self.pending[0] else "error",
                                                "detail": "displayed" if self.pending[0]
                                                else self.pending[3]})
                self.pending = None
                if self.after_upload is not None:
                    self._commit(self.after_upload, "upload")
                    self.after_upload = None
                elif self.queued:
                    job, self.queued = self.queued, None
                    self._begin(job)
        return False

    def call(self, method, path, obj=None, body=None, timeout=8.0):
        if self.dark:
            self.dark -= 1
            return None, b"web server down"
        if method == "GET":
            return self._get(path)
        return self._post(path, obj, body)

    def wait_info(self, seconds):
        end = soak_long.time.time() + seconds
        while True:
            status, data = self.call("GET", "/api/v1/system/getDevInfo")
            if status == 200:
                return json.loads(data)
            if soak_long.time.time() >= end:
                return None
            soak_long.time.sleep(2)

    def _get(self, path):
        if path == "/api/v1/system/getDevInfo":
            prov = self._tick()
            return 200, json.dumps({
                "uptime_s": self.up, "mqttConnected": self.mqtt,
                "mqttReason": "connected" if self.mqtt else "deferred_heap",
                "displayBusy": bool(self.busy_until), "lastDisplayOk": self.ok, "fwVersion": FW,
                "otaPartition": self.slot, "otaPendingVerify": False, "softapUp": False,
                "bleProvUp": prov}).encode()
        if path == "/api/v1/system/getDiag":
            heap = {"free": 1, "minFree": 1, "freeInt": 20000, "minFreeInt": 8, "largestInt": 12800,
                    "freeSpiram": 1, "largestSpiram": 1}
            return 200, json.dumps({"heap": heap, "stackMin": {"httpd": 2000}}).encode()
        if path == "/api/v1/display/getLastFrameMeta":
            return 200, json.dumps({"present": True, "seq": self.seq,
                                    "crc32": zlib.crc32(self.bitmap) & 0xFFFFFFFF,
                                    "savedUs": self.saved_up * 1_000_000, "bytes": W,
                                    "source": self.source}).encode()
        if path == "/api/v1/display/lastFrame":
            return 200, self.bitmap
        if path == "/api/v1/system/eventLog":
            return 200, ("T+0 BOOT running wifi=1\n" + self.prov_line).encode()
        if path == "/":
            return 200, b"<html>" + b" " * 2000 + b"</html>"
        if path == "/api/v1/network/getMqttParam":
            return 200, json.dumps({"clientId": "ek410-test", "topicUp": "t/up",
                                    "topicDown": "t/down", "hasPassword": True}).encode()
        if path.endswith(("getPowerParam", "getLastFrameMeta", "getWifiParam")):
            return 200, b"{}"
        return 404, b"{}"

    def _post(self, path, obj, body):
        if path == "/api/v1/display/uploadFrame":
            if self.busy_until:                 # refused at once; the body is discarded
                if self.flags.get("upload_waits"):
                    self.after_upload = body
                    return 200, b'{"code":0,"msg":"uploaded"}'
                return 200, b'{"code":1,"msg":"panel busy"}'
            self._commit(body, "upload")
            return 200, b'{"code":0,"msg":"uploaded"}'
        if path == "/api/v1/display/displayUrl":
            self.start(self.name_of(obj["url"]), obj["url"])
        elif path == "/api/v1/display/renderCanvas":
            self.start("canvas")
        elif path == "/api/v1/display/renderText":
            self.start("text")
        elif path == "/api/v1/display/proxyImage":
            return 200, b"\xff\xd8" + bytes(2000)
        else:
            return 404, b"{}"
        return 200, b'{"code":0,"msg":"queued"}'


class FakeBroker:
    """The unit's broker as the soak sees it: acknowledgements, telemetry."""
    flags: dict = {}
    current = None

    def __init__(self, secret, client_id, topic_up, topic_down):
        self.state, self.connected, self.host = {"up": True, "lost": 0}, True, "broker.test"
        self.up, self.down, self.acks = topic_up, topic_down, []
        FakeBroker.current = self

    def publish(self, cmd, params=None):
        if cmd == "ping":
            if not self.flags.get("ping_lost"):
                self.acks.append({"last_cmd": "ping", "cmd_status": "ok", "detail": "pong"})
            return True
        dev = FakeDevice.dev
        if cmd == "display_url":
            dev.start(dev.name_of(params["url"]), params["url"], cmd)
        else:
            dev.start({"render_canvas": "canvas", "render_text": "text"}[cmd], None, cmd)
        self.acks.append({"last_cmd": cmd, "cmd_status": "queued", "detail": "queued"})
        return True

    def drop_acks_before(self, t):
        pass

    def wait_ack(self, cmd, statuses, seconds, not_before=None):
        for i, a in enumerate(self.acks):
            if a["last_cmd"] == cmd and a["cmd_status"] in statuses:
                del self.acks[i]
                return soak_long.time.time(), a
        return None

    def wait_telemetry(self, seconds, not_before=None):
        soak_long.time.sleep(min(seconds, soak_long.TELEMETRY_S))
        dev = FakeDevice.dev
        crc = zlib.crc32(dev.bitmap) & 0xFFFFFFFF
        bmp = bytearray(soak_long.PREVIEW_BMP)
        bmp[0:2], bmp[18:22], bmp[22:26] = b"BM", (400).to_bytes(4, "little"), (300).to_bytes(4, "little")
        return soak_long.time.time(), {
            "type": "telemetry", "deviceId": "test", "fwVersion": FW, "uptime_s": dev.up,
            "ip": HOST, "frame_crc32": crc ^ (1 if self.flags.get("lie_crc") else 0),
            "frame_preview_w": 400, "frame_preview_h": 300,
            "frame_preview_b64": base64.b64encode(bytes(bmp)).decode()}

    def close(self):
        pass


def fake_upload(host, data):
    dev = FakeDevice.dev
    dev.slot = "ota_1" if dev.slot == "ota_0" else "ota_0"
    dev.up, dev.saved_up, dev.mqtt, dev.busy_until = 0, 0, False, 0.0
    dev.saved = None                   # nothing saved since this boot
    dev.pending = dev.queued = None
    dev.ok, dev.bitmap, dev.source = True, frame_for("boot"), "url"
    FakeDevice.dev.mqtt = True            # up again two polls later, as after a boot
    soak_long.time.t += 100               # an upload and the boot paint take their time
    return 200, b'{"code":0}'


def run(out, image, secret, flags, www_from=None, rounds=48):
    """One soak against the simulation. (exit code, summary, the log lines)"""
    FakeDevice.flags = FakeBroker.flags = flags
    lines = []
    soak_long.Device, soak_long.BrokerMirror = FakeDevice, FakeBroker
    soak_long.upload_firmware = fake_upload
    soak_long.time, soak_long.log = FakeClock(), lines.append
    if www_from and not os.path.exists(os.path.join(out, "www")):
        shutil.copytree(www_from, os.path.join(out, "www"))
    sys.argv = ["soak_long.py", "--host", HOST, "--out", out, "--rounds", str(rounds),
                "--interval", "20", "--upload-every", "10", "--reject-every", "5",
                "--fault-every", "7", "--content-every", "3", "--burst-every", "1",
                "--web-every", "6", "--https-url", "https://example.invalid/p.jpg",
                "--https-every", "9", "--ota-every-hours", "0.05", "--image", image,
                "--broker-secret", secret, "--mqtt-every", "4", "--ping-every", "7",
                "--server-ip", "127.0.0.1", "--no-caffeinate"]
    try:
        code = soak_long.main()
    except SystemExit as e:               # the tool's own refusals to start
        code, e = 3, f"SystemExit {e}"
        lines.append(str(e))
    with open(os.path.join(out, "summary.json"), encoding="utf-8") as f:
        return code, json.load(f), lines


def main():
    fails, n_cases = [], 0
    tmp = tempfile.mkdtemp(prefix="ek410_soak_dry_")
    try:
        image, secret = os.path.join(tmp, "fw.bin"), os.path.join(tmp, "broker.json")
        with open(image, "wb") as f:
            f.write(b"firmware")
        with open(secret, "w", encoding="utf-8") as f:
            json.dump({"host": "broker.test", "port": 1883, "username": "u", "password": "p"}, f)

        # the image set once (every run checks it against the same raw frame)
        www = os.path.join(tmp, "www")
        soak_long.log = lambda msg: None
        soak_long.build_image_set(Path(www))
        FakeDevice.raw = (open(os.path.join(www, "raw.bin"), "rb").read())
        if len(FakeDevice.raw) != W:
            fails.append(f"raw.bin is {len(FakeDevice.raw)} bytes")

        # ---- everything as it should be
        out = os.path.join(tmp, "good")
        code, s, lines = run(out, image, secret, {"prov_after": 14}, www_from=www)
        if s.get("ok") is not True or code != 0:
            fails.append(f"good run: ok={s.get('ok')} exit={code} unexpected="
                         f"{json.dumps(s.get('unexpected'), ensure_ascii=False)[:600]}")
        want = {"displayed": 1, "canvas": 1, "text": 1, "uploads": 1, "https": 1,
                "faults_expected": 1, "rejected_expected": 1, "burst_rounds": 4,
                "frame_checks": 10, "planned_ota": 1, "mqtt_rounds": 1, "pings": 1,
                "telemetry_checks": 10, "prov_sessions": 1}
        low = [k for k, n in want.items() if (s.get(k) or 0) < n]
        if low:
            fails.append("good run did not exercise: " + ", ".join(f"{k}={s.get(k)}" for k in low))
        patterns = {json.loads(l)["burst"]["pattern"] for l in open(os.path.join(out, "rounds.jsonl"), encoding="utf-8")
                    if '"burst"' in l and l.startswith("{\"round\"")}
        if patterns != set(soak_long.BURST_PATTERNS):
            fails.append(f"burst patterns seen: {sorted(patterns)}")

        # ---- one thing broken at a time: the verdict has to find each
        cases = [
            ("MQTT dead after provisioning", {"prov_after": 14, "prov_breaks_mqtt": True},
             ("prov_exit_failures", "mqtt_down_gaps")),
            ("canvas not drawn the same twice", {"canvas_drifts": True}, ("frame_problems",)),
            ("telemetry with the wrong frame", {"lie_crc": True}, ("telemetry_problems",)),
            ("broker never told how a command ended", {"swallow_done": True}, ("mqtt_ack_missing",)),
            ("ping not answered", {"ping_lost": True}, ("ping_failed",)),
            ("an upload waiting behind a refresh", {"upload_waits": True}, ("burst_anomalies",)),
            ("a display that saved nothing new", {"no_save": True}, ("frame_problems",)),
        ]
        n_cases = len(cases)
        for title, flags, counters in cases:
            out = os.path.join(tmp, "case%d" % len(fails))
            code, s, lines = run(out, image, secret, flags, www_from=www)
            if s.get("ok") is not False or code != 1:
                fails.append(f"{title}: verdict ok={s.get('ok')} exit={code}")
            missed = [c for c in counters if not s.get(c)]
            if missed:
                fails.append(f"{title}: not counted: {missed} — "
                             f"{json.dumps(s.get('unexpected'), ensure_ascii=False)[:400]}")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    for f in fails:
        print("FAIL " + f)
    print("RESULT: " + ("fail — %d" % len(fails) if fails else
                       "pass — every kind of round ran clean, %d injected faults all found" % n_cases))
    raise SystemExit(1 if fails else 0)


if __name__ == "__main__":
    main()
