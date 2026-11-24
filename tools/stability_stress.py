#!/usr/bin/env python3
"""
EK410 boundary / stability stress harness.

Covers SoftAP, MQTT (HTTP + downlink cmds), display (text/canvas/upload),
Wi-Fi scan, device rename, power modes (safe), bad payloads, and stacked
business flows. Watches USB serial for panic / reboot.

Does NOT: factory-reset, deep-sleep/sleepNow, OTA, reboot cmd, hibernate.

Usage:
  python3 tools/stability_stress.py [--host 192.168.31.42] [--rounds 6] [--port /dev/cu.wchusbserial3130]
"""
from __future__ import annotations

import argparse
import json
import re
import sys
import threading
import time
import urllib.error
import urllib.request
from dataclasses import dataclass, field
from typing import Any, Optional


PANIC_RE = re.compile(
    r"(Guru Meditation|Load access fault|Store access fault|panic'ed|"
    r"Stack canary|assert failed|Brownout|cache error|Guru)",
    re.I,
)
REBOOT_RE = re.compile(r"(rst:0x|Rebooting\.\.\.|POWERON|Calling app_main)", re.I)


@dataclass
class SerialWatch:
    port: str
    baud: int = 115200
    lines: list[str] = field(default_factory=list)
    panics: list[str] = field(default_factory=list)
    reboots: int = 0
    _stop: bool = False
    _thr: Optional[threading.Thread] = None

    def start(self) -> None:
        try:
            import serial  # type: ignore
        except ImportError as e:
            raise SystemExit(f"pyserial required: {e}") from e

        ser = serial.Serial(self.port, self.baud, timeout=0.2)

        def run() -> None:
            while not self._stop:
                try:
                    raw = ser.read(8192)
                except Exception:
                    time.sleep(0.2)
                    continue
                if not raw:
                    continue
                text = raw.decode("utf-8", "replace")
                for line in text.splitlines():
                    self.lines.append(line)
                    if PANIC_RE.search(line):
                        self.panics.append(line)
                    if "Rebooting" in line or "Calling app_main" in line:
                        self.reboots += 1

        self._thr = threading.Thread(target=run, daemon=True)
        self._thr.start()
        self._ser = ser

    def stop(self) -> None:
        self._stop = True
        if self._thr:
            self._thr.join(timeout=2)
        try:
            self._ser.close()
        except Exception:
            pass


class Api:
    def __init__(self, host: str, timeout: float = 12.0):
        self.base = f"http://{host}"
        self.timeout = timeout

    def get(self, path: str) -> Any:
        with urllib.request.urlopen(self.base + path, timeout=self.timeout) as r:
            return json.loads(r.read().decode())

    def post(self, path: str, body: Optional[dict] = None) -> Any:
        data = json.dumps(body or {}).encode()
        req = urllib.request.Request(
            self.base + path,
            data=data,
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        with urllib.request.urlopen(req, timeout=self.timeout) as r:
            raw = r.read().decode()
            return json.loads(raw) if raw else {}

    def post_raw(self, path: str, data: bytes, content_type: str = "application/octet-stream", timeout: Optional[float] = None) -> Any:
        req = urllib.request.Request(
            self.base + path,
            data=data,
            headers={"Content-Type": content_type},
            method="POST",
        )
        with urllib.request.urlopen(req, timeout=timeout or max(self.timeout, 60.0)) as r:
            raw = r.read().decode()
            return json.loads(raw) if raw else {}

    def get_text(self, path: str) -> str:
        with urllib.request.urlopen(self.base + path, timeout=self.timeout) as r:
            return r.read().decode("utf-8", "replace")

    def post_expect_fail(self, path: str, body: Any) -> str:
        data = json.dumps(body).encode() if not isinstance(body, (bytes, bytearray)) else body
        req = urllib.request.Request(
            self.base + path,
            data=data,
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as r:
                return f"unexpected_ok:{r.status}:{r.read()[:120]!r}"
        except urllib.error.HTTPError as e:
            return f"http_{e.code}"
        except Exception as e:
            return f"err:{type(e).__name__}"


@dataclass
class Stats:
    ok: int = 0
    fail: int = 0
    notes: list[str] = field(default_factory=list)

    def mark(self, name: str, passed: bool, detail: str = "") -> None:
        if passed:
            self.ok += 1
            print(f"  OK  {name}" + (f" — {detail}" if detail else ""))
        else:
            self.fail += 1
            msg = f"FAIL {name}" + (f" — {detail}" if detail else "")
            self.notes.append(msg)
            print(f"  {msg}")


def wait_sta(api: Api, seconds: float = 25.0) -> dict:
    deadline = time.time() + seconds
    last: dict = {}
    while time.time() < deadline:
        try:
            last = api.get("/api/v1/system/getDevInfo")
            if last.get("wifiConnected") and last.get("ip"):
                return last
        except Exception:
            pass
        time.sleep(1.0)
    return last


def case_info_spam(api: Api, st: Stats, n: int = 20) -> None:
    print("\n[case] info spam")
    for i in range(n):
        try:
            api.get("/api/v1/system/getDevInfo")
            api.get("/api/v1/network/getMqttParam")
            api.get("/api/v1/network/getWifiParam")
            api.get("/api/v1/system/getPowerParam")
            st.mark(f"info#{i}", True)
        except Exception as e:
            st.mark(f"info#{i}", False, str(e))
            break


def case_softap_cycle(api: Api, st: Stats, watch: SerialWatch, rounds: int) -> None:
    print("\n[case] SoftAP open/close cycles")
    for i in range(rounds):
        before_panic = len(watch.panics)
        before_reboot = watch.reboots
        try:
            r = api.post("/api/v1/network/openSoftAp", {})
            time.sleep(2.5)
            info = api.get("/api/v1/system/getDevInfo")
            up = bool(info.get("softapUp"))
            # STA may briefly hiccup on APSTA; allow recovery
            time.sleep(1.0)
            api.post("/api/v1/network/closeSoftAp", {})
            time.sleep(2.0)
            info2 = wait_sta(api, 20)
            closed = not bool(info2.get("softapUp"))
            wifi_ok = bool(info2.get("wifiConnected"))
            panic = len(watch.panics) > before_panic
            reboot = watch.reboots > before_reboot
            st.mark(
                f"softap#{i}",
                up and closed and wifi_ok and not panic and not reboot,
                f"up={up} closed={closed} wifi={wifi_ok} panic={panic} reboot={reboot} open={r}",
            )
        except Exception as e:
            st.mark(f"softap#{i}", False, str(e))
            wait_sta(api, 25)


def case_mqtt_boundary(api: Api, st: Stats, rounds: int) -> None:
    print("\n[case] MQTT param / test boundaries")
    base = api.get("/api/v1/network/getMqttParam")
    host = base.get("host") or ""
    user = base.get("username") or ""
    password = base.get("password") or ""
    good = {
        "host": host,
        "port": int(base.get("port") or 8883),
        "transport": "mqtt",
        "qos": 1,
        "username": user,
        "password": password,
    }
    for i in range(rounds):
        try:
            # valid save + test
            api.post("/api/v1/network/setMqttParam", good)
            time.sleep(1.0)
            t = api.post("/api/v1/network/testMqtt", {})
            st.mark(f"mqtt_test#{i}", bool(t.get("ok") or t.get("connected") or t.get("code") == 0), str(t)[:120])

            # Flip transport briefly then restore. mqtts may fail against a
            # plain broker — failure is OK; panic/reboot is not.
            flip = dict(good)
            flip["transport"] = "mqtts"
            flip["port"] = 8883
            api.post("/api/v1/network/setMqttParam", flip)
            time.sleep(2.0)
            try:
                api.post("/api/v1/network/testMqtt", {})
            except Exception:
                pass
            api.post("/api/v1/network/setMqttParam", good)
            time.sleep(2.0)
            t2 = api.post("/api/v1/network/testMqtt", {})
            st.mark(
                f"mqtt_flip#{i}",
                bool(t2.get("ok") or t2.get("connected") or t2.get("code") == 0),
                str(t2)[:120],
            )
        except Exception as e:
            st.mark(f"mqtt#{i}", False, str(e))
            try:
                api.post("/api/v1/network/setMqttParam", good)
            except Exception:
                pass


def case_mqtt_bad_payloads(api: Api, st: Stats, restore: dict) -> None:
    print("\n[case] MQTT bad payloads (must reject, not crash)")
    trials = [
        ("empty_host", {"host": "", "port": 1883, "transport": "mqtt"}),
        ("long_host", {"host": "x" * 80, "port": 1883, "transport": "mqtt"}),
        ("bad_port", {"host": restore.get("host") or "example.invalid", "port": 99999, "transport": "mqtt"}),
        ("long_user", {"host": restore.get("host") or "example.invalid", "port": 1883, "username": "u" * 80}),
    ]
    for name, body in trials:
        detail = api.post_expect_fail("/api/v1/network/setMqttParam", body)
        # device may return 200 with code!=0 or HTTP error — either is fine if alive
        alive = False
        rejected = ("code\":1" in detail) or detail.startswith("http_") or "invalid" in detail
        try:
            api.get("/api/v1/system/getDevInfo")
            alive = True
        except Exception:
            alive = False
        st.mark(f"mqtt_bad:{name}", alive and (rejected or name == "bad_port"), detail)
    # Always restore working broker — bad payloads must not stick.
    try:
        api.post("/api/v1/network/setMqttParam", restore)
        time.sleep(1.5)
    except Exception as e:
        st.mark("mqtt_restore_after_bad", False, str(e))
        return
    st.mark("mqtt_restore_after_bad", True)


def case_display(api: Api, st: Stats, rounds: int) -> None:
    print("\n[case] display render boundaries")
    for i in range(rounds):
        try:
            api.post(
                "/api/v1/display/renderText",
                {"text": f"stress {i}\nline2", "color": "black", "bg": "white", "scale": 2 + (i % 2)},
            )
            time.sleep(0.8)
            api.post("/api/v1/display/testPattern", {})
            time.sleep(1.2)
            api.post(
                "/api/v1/display/renderCanvas",
                {
                    "bg": "white",
                    "items": [
                        {"type": "text", "x": 40, "y": 40, "text": f"cv{i}", "color": "black", "scale": 3},
                        {"type": "rect", "x": 100, "y": 100, "w": 200, "h": 80, "color": "red", "fill": False},
                    ],
                },
            )
            time.sleep(1.5)
            st.mark(f"display#{i}", True)
        except Exception as e:
            # render may return busy — still count as soft fail if device alive
            try:
                api.get("/api/v1/system/getDevInfo")
                st.mark(f"display#{i}", False, f"alive_but {e}")
            except Exception as e2:
                st.mark(f"display#{i}", False, f"dead {e2} / {e}")


def case_wifi_scan(api: Api, st: Stats, rounds: int) -> None:
    print("\n[case] Wi-Fi scan")
    for i in range(rounds):
        try:
            # SoftAP may be off; scan on STA
            r = api.get("/api/v1/network/scanWifi")
            aps = r.get("aps") if isinstance(r, dict) else None
            st.mark(f"scan#{i}", isinstance(aps, list), f"n={len(aps) if isinstance(aps, list) else '?'}")
            time.sleep(1.0)
        except Exception as e:
            st.mark(f"scan#{i}", False, str(e))


def case_softap_plus_display(api: Api, st: Stats, watch: SerialWatch) -> None:
    print("\n[case] SoftAP open + display while AP up")
    before = len(watch.panics)
    before_reboot = watch.reboots
    try:
        api.post("/api/v1/network/openSoftAp", {})
        time.sleep(4.0)  # wait APSTA + mqtt_stop settle
        info = api.get("/api/v1/system/getDevInfo")
        api.post("/api/v1/display/renderText", {"text": "AP+draw", "scale": 3})
        time.sleep(3.0)
        api.post("/api/v1/network/closeSoftAp", {})
        time.sleep(3.0)
        wait_sta(api, 25)
        st.mark(
            "softap+display",
            bool(info.get("softapUp"))
            and len(watch.panics) == before
            and watch.reboots == before_reboot,
            f"ap={info.get('softapUp')} panics+={len(watch.panics)-before} reboot+={watch.reboots-before_reboot}",
        )
    except Exception as e:
        st.mark("softap+display", False, str(e))
        wait_sta(api, 25)


def case_idempotent_softap(api: Api, st: Stats, watch: SerialWatch) -> None:
    print("\n[case] SoftAP double-open / double-close")
    before = len(watch.panics)
    try:
        api.post("/api/v1/network/openSoftAp", {})
        time.sleep(1.5)
        api.post("/api/v1/network/openSoftAp", {})
        time.sleep(1.0)
        info = api.get("/api/v1/system/getDevInfo")
        api.post("/api/v1/network/closeSoftAp", {})
        time.sleep(1.0)
        api.post("/api/v1/network/closeSoftAp", {})
        time.sleep(2.0)
        wait_sta(api, 20)
        st.mark(
            "softap_idempotent",
            bool(info.get("softapUp")) and len(watch.panics) == before,
            f"panics+={len(watch.panics)-before}",
        )
    except Exception as e:
        st.mark("softap_idempotent", False, str(e))
        wait_sta(api, 25)


def case_power_param_safe(api: Api, st: Stats) -> None:
    print("\n[case] power param (always / cycle set — no sleepNow)")
    try:
        cur = api.get("/api/v1/system/getPowerParam")
        api.post("/api/v1/system/setPowerParam", {"mode": "always", "sleepMin": 0, "wakeUrl": ""})
        time.sleep(0.4)
        again = api.get("/api/v1/system/getPowerParam")
        st.mark("power_always", again.get("mode") == "always", str(again)[:100])
        # Set cycle with long period then immediately restore always (do not wait for sleep).
        api.post("/api/v1/system/setPowerParam", {"mode": "cycle", "sleepMin": 120, "wakeUrl": ""})
        time.sleep(0.5)
        cyc = api.get("/api/v1/system/getPowerParam")
        st.mark("power_cycle_set", cyc.get("mode") == "cycle" and int(cyc.get("sleepMin") or 0) == 120, str(cyc)[:100])
        api.post("/api/v1/system/setPowerParam", {"mode": "always", "sleepMin": 0, "wakeUrl": ""})
        time.sleep(0.4)
        back = api.get("/api/v1/system/getPowerParam")
        st.mark("power_restore_always", back.get("mode") == "always", str(back)[:100])
        if isinstance(cur, dict) and cur.get("mode"):
            api.post(
                "/api/v1/system/setPowerParam",
                {
                    "mode": cur.get("mode", "always"),
                    "sleepMin": cur.get("sleepMin", 0),
                    "wakeUrl": cur.get("wakeUrl", ""),
                },
            )
    except Exception as e:
        st.mark("power_param", False, str(e))
        try:
            api.post("/api/v1/system/setPowerParam", {"mode": "always", "sleepMin": 0, "wakeUrl": ""})
        except Exception:
            pass


def case_devinfo_rename(api: Api, st: Stats) -> None:
    print("\n[case] setDevInfo rename + restore")
    try:
        info = api.get("/api/v1/system/getDevInfo")
        orig = info.get("devName") or "EK410"
        tmp = (orig[:20] + "-T")[:32]
        api.post("/api/v1/system/setDevInfo", {"devName": tmp})
        time.sleep(0.4)
        mid = api.get("/api/v1/system/getDevInfo")
        api.post("/api/v1/system/setDevInfo", {"devName": orig})
        time.sleep(0.4)
        end = api.get("/api/v1/system/getDevInfo")
        st.mark("dev_rename", mid.get("devName") == tmp and end.get("devName") == orig, f"{orig}→{tmp}→{end.get('devName')}")
        # bad rename must not crash
        detail = api.post_expect_fail("/api/v1/system/setDevInfo", {"devName": ""})
        alive = True
        try:
            api.get("/api/v1/system/getDevInfo")
        except Exception:
            alive = False
        st.mark("dev_rename_empty", alive, detail)
    except Exception as e:
        st.mark("dev_rename", False, str(e))


def case_web_root(api: Api, st: Stats) -> None:
    print("\n[case] web UI root")
    try:
        html = api.get_text("/")
        st.mark("web_root", "EK410" in html and "<html" in html.lower(), f"len={len(html)}")
    except Exception as e:
        st.mark("web_root", False, str(e))


def case_display_bad(api: Api, st: Stats) -> None:
    print("\n[case] display bad payloads")
    trials = [
        ("missing_text", "/api/v1/display/renderText", {"color": "black"}),
        ("bad_json_canvas", "/api/v1/display/renderCanvas", {"items": "nope"}),
        ("empty_items", "/api/v1/display/renderCanvas", {"bg": "white", "items": []}),
    ]
    for name, path, body in trials:
        detail = api.post_expect_fail(path, body)
        alive = False
        try:
            api.get("/api/v1/system/getDevInfo")
            alive = True
        except Exception:
            pass
        st.mark(f"disp_bad:{name}", alive, detail)
    # wrong-size uploadFrame
    try:
        api.post_raw("/api/v1/display/uploadFrame", b"too-small", timeout=10)
        detail = "unexpected_ok"
        ok = False
    except urllib.error.HTTPError as e:
        detail = f"http_{e.code}"
        ok = True
    except Exception as e:
        # device often returns 200 with {"code":1,...}
        detail = str(e)
        ok = True
        try:
            # retry via expect-style: short post with content-length mismatch hard in urllib
            pass
        except Exception:
            pass
    try:
        # Explicit short body through Request with Content-Length
        import urllib.request as ur
        req = ur.Request(
            api.base + "/api/v1/display/uploadFrame",
            data=b"x" * 100,
            headers={"Content-Type": "application/octet-stream"},
            method="POST",
        )
        with ur.urlopen(req, timeout=10) as r:
            raw = r.read().decode()
            detail = raw[:120]
            ok = ("code\":1" in raw) or ("must be" in raw)
    except urllib.error.HTTPError as e:
        detail = f"http_{e.code}"
        ok = True
    except Exception as e:
        detail = f"{type(e).__name__}:{e}"
        ok = False
    alive = True
    try:
        api.get("/api/v1/system/getDevInfo")
    except Exception:
        alive = False
    st.mark("disp_bad:upload_short", alive and ok, detail)


def wait_panel_idle(api: Api, timeout: float = 90.0) -> bool:
    """Wait until the render queue accepts work, then give the panel time to finish.

    A successful queue does not mean the SPI refresh is done (20–40s). Upload
    needs the panel lock, so we wait an extra refresh window after queue OK.
    """
    deadline = time.time() + timeout
    queued = False
    while time.time() < deadline:
        try:
            r = api.post("/api/v1/display/renderText", {"text": ".", "scale": 1})
            if isinstance(r, dict) and r.get("code") == 0:
                queued = True
                break
            if "busy" in str(r).lower():
                time.sleep(2.0)
                continue
            time.sleep(1.0)
        except Exception:
            time.sleep(1.0)
    if not queued:
        # Queue stayed busy — still wait out a full refresh before upload tries
        time.sleep(min(35.0, max(0.0, deadline - time.time())))
        return False
    # Panel refresh after queue typically 20–40s on EK410
    time.sleep(35.0)
    return True


def case_upload_frame(api: Api, st: Stats, watch: SerialWatch) -> None:
    print("\n[case] uploadFrame 960000-byte raw")
    before = len(watch.panics)
    wait_panel_idle(api, 90.0)
    try:
        # 1600x1200 / 2 packed nibbles = 960000 — solid light gray 0x11
        frame = bytes([0x11]) * 960000
        r = None
        for attempt in range(6):
            r = api.post_raw("/api/v1/display/uploadFrame", frame, timeout=120)
            if isinstance(r, dict) and r.get("code") == 0:
                break
            if "busy" in str(r).lower():
                time.sleep(8.0)
                continue
            break
        time.sleep(4.0)
        alive = True
        try:
            api.get("/api/v1/system/getDevInfo")
        except Exception:
            alive = False
        st.mark(
            "upload_frame",
            alive and len(watch.panics) == before and isinstance(r, dict) and r.get("code") == 0,
            f"{r} panics+={len(watch.panics)-before}",
        )
    except Exception as e:
        st.mark("upload_frame", False, str(e))


def case_mqtt_downlink(api: Api, st: Stats, watch: SerialWatch, mqtt: dict, cli_password: str = "") -> None:
    print("\n[case] MQTT downlink commands (ping/clear/render_text/set_power/unknown)")
    try:
        import paho.mqtt.client as mqtt_lib  # type: ignore
    except ImportError:
        st.mark("mqtt_downlink", False, "paho-mqtt missing (pip install paho-mqtt)")
        return

    host = mqtt.get("host") or ""
    port = int(mqtt.get("port") or 8883)
    transport = (mqtt.get("transport") or "mqtt").lower()
    user = mqtt.get("username") or ""
    password = cli_password or mqtt.get("password") or ""
    cid = mqtt.get("clientId") or ""
    topic = mqtt.get("topicDown") or ""
    if not topic and cid:
        did = cid[6:] if cid.lower().startswith("ek410-") else cid
        topic = f"device/ek410_Eink/{did}/downlink"
    if not host or not topic:
        st.mark("mqtt_downlink", False, "mqtt host/topicDown empty")
        return
    if user and mqtt.get("hasPassword") and not password:
        # The device no longer reveals its password: without it this client
        # cannot authenticate, which is a missing input, not a device fault.
        print("  SKIP mqtt_downlink — stored MQTT password is write-only; pass --mqtt-password to run")
        return
    # Ensure connected
    try:
        info = api.get("/api/v1/system/getDevInfo")
        if not info.get("mqttConnected"):
            api.post("/api/v1/network/testMqtt", {})
            time.sleep(2)
    except Exception as e:
        st.mark("mqtt_downlink", False, f"pre {e}")
        return

    before = len(watch.panics)

    def pub(payload: dict) -> tuple[bool, str]:
        # Broker may serve plain MQTT on 8883; mosquitto_pub auto-TLS there fails.
        client = mqtt_lib.Client(
            mqtt_lib.CallbackAPIVersion.VERSION2,
            client_id=f"stress-{int(time.time()*1000)%100000}",
        )
        if user:
            client.username_pw_set(user, password)
        if transport in ("mqtts", "tls", "ssl"):
            client.tls_set()
        err: list[str] = []
        try:
            client.connect(host, port, keepalive=20)
            client.loop_start()
            info = client.publish(topic, json.dumps(payload), qos=1)
            info.wait_for_publish(timeout=6)
            ok = info.rc == mqtt_lib.MQTT_ERR_SUCCESS and info.is_published()
            client.loop_stop()
            client.disconnect()
            return ok, f"rc={info.rc} published={info.is_published()}"
        except Exception as e:
            err.append(str(e))
            try:
                client.loop_stop()
                client.disconnect()
            except Exception:
                pass
            return False, err[0][:120]

    cmds = [
        ("ping", {"cmd": "ping"}),
        ("clear", {"cmd": "clear", "params": {"color": "white"}}),
        ("render_text", {"cmd": "render_text", "params": {"text": "MQTT OK", "scale": 3, "color": "black", "bg": "white"}}),
        ("set_power_always", {"cmd": "set_power", "params": {"mode": "always"}}),
        ("unknown", {"cmd": "no_such_cmd"}),
    ]
    for name, payload in cmds:
        panics_before = len(watch.panics)
        reboots_before = watch.reboots
        ok, detail = pub(payload)
        # clear/render can take a few seconds on panel; allow recovery window
        time.sleep(3.0 if name in ("clear", "render_text") else 1.2)
        alive = False
        try:
            info = wait_sta(api, 35)
            alive = bool(info.get("wifiConnected"))
            if alive and not info.get("mqttConnected"):
                try:
                    api.post("/api/v1/network/testMqtt", {})
                except Exception:
                    pass
        except Exception:
            alive = False
        no_panic = len(watch.panics) == panics_before
        no_reboot = watch.reboots == reboots_before
        st.mark(
            f"mqtt_cmd:{name}",
            ok and alive and no_panic and no_reboot,
            f"{detail} alive={alive} panic+={len(watch.panics)-panics_before} reboot+={watch.reboots-reboots_before}",
        )
    # drain any panel work from downlink before later display cases
    wait_panel_idle(api, 60.0)
    if len(watch.panics) > before:
        st.mark("mqtt_downlink_no_panic", False, f"panics+={len(watch.panics)-before}")
    else:
        st.mark("mqtt_downlink_no_panic", True, "ok")


def case_render_busy(api: Api, st: Stats) -> None:
    print("\n[case] rapid render (busy path)")
    try:
        busy_hits = 0
        for i in range(6):
            try:
                r = api.post("/api/v1/display/renderText", {"text": f"busy{i}", "scale": 2})
                if isinstance(r, dict) and r.get("code") == 0:
                    pass
            except urllib.error.HTTPError:
                busy_hits += 1
            except Exception as e:
                # send_err may be 200 with code 1
                if "busy" in str(e).lower():
                    busy_hits += 1
        # Also fire overlapping posts without waiting — collect responses via raw
        import concurrent.futures

        def one(i: int) -> str:
            try:
                r = api.post("/api/v1/display/testPattern", {})
                return json.dumps(r)[:60]
            except Exception as e:
                return str(e)[:60]

        with concurrent.futures.ThreadPoolExecutor(max_workers=3) as ex:
            results = list(ex.map(one, range(3)))
        alive = True
        try:
            api.get("/api/v1/system/getDevInfo")
        except Exception:
            alive = False
        time.sleep(3.0)
        st.mark("render_busy", alive, f"results={results}")
    except Exception as e:
        st.mark("render_busy", False, str(e))


def case_softap_scan_info(api: Api, st: Stats, watch: SerialWatch) -> None:
    print("\n[case] SoftAP up + scanWifi + getDevInfo")
    before = len(watch.panics)
    try:
        api.post("/api/v1/network/openSoftAp", {})
        time.sleep(3.5)
        info = api.get("/api/v1/system/getDevInfo")
        scan = api.get("/api/v1/network/scanWifi")
        api.post("/api/v1/network/closeSoftAp", {})
        time.sleep(2.5)
        wait_sta(api, 25)
        aps = scan.get("aps") if isinstance(scan, dict) else None
        st.mark(
            "softap+scan",
            bool(info.get("softapUp")) and isinstance(aps, list) and len(watch.panics) == before,
            f"aps={len(aps) if isinstance(aps, list) else '?'} panics+={len(watch.panics)-before}",
        )
    except Exception as e:
        st.mark("softap+scan", False, str(e))
        wait_sta(api, 25)


def case_wifi_wrong_pass_restore(api: Api, st: Stats) -> None:
    print("\n[case] Wi-Fi wrong password then restore saved")
    try:
        wf = api.get("/api/v1/network/getWifiParam")
        ssid = wf.get("ssid") or ""
        if not ssid:
            st.mark("wifi_wrong_pass", False, "no saved ssid")
            return
        # Device tries connect ~20s then restores NVS; HTTP may error/timeout.
        detail = ""
        try:
            old_timeout = api.timeout
            api.timeout = 35.0
            r = api.post("/api/v1/network/setWifiParam", {"ssid": ssid, "password": "__wrong_pass_stress__"})
            api.timeout = old_timeout
            detail = str(r)[:100]
        except Exception as e:
            api.timeout = 15.0
            detail = f"exc={type(e).__name__}:{e}"
        info = wait_sta(api, 40)
        st.mark(
            "wifi_wrong_pass",
            bool(info.get("wifiConnected")) and info.get("wifiSsid") == ssid,
            f"{detail} wifi={info.get('wifiConnected')} ssid={info.get('wifiSsid')}",
        )
    except Exception as e:
        info = wait_sta(api, 40)
        st.mark("wifi_wrong_pass", bool(info.get("wifiConnected")), f"exc={e} recovered={info.get('wifiConnected')}")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="192.168.31.42")
    ap.add_argument("--port", default="/dev/cu.wchusbserial3130")
    ap.add_argument("--rounds", type=int, default=5)
    ap.add_argument("--skip-upload", action="store_true", help="skip 960KB frame upload")
    ap.add_argument("--skip-wifi-wrong", action="store_true", help="skip wrong-password Wi-Fi try")
    ap.add_argument(
        "--mqtt-password",
        default="",
        help="MQTT password for the broker-downlink case (getMqttParam only reports hasPassword)",
    )
    args = ap.parse_args()

    watch = SerialWatch(args.port)
    watch.start()
    api = Api(args.host, timeout=15.0)
    st = Stats()

    print(f"EK410 stability stress host={args.host} rounds={args.rounds}")
    info = wait_sta(api, 30)
    if not info.get("wifiConnected"):
        print("Device not reachable on STA — abort")
        watch.stop()
        return 2
    if info.get("softapUp"):
        try:
            api.post("/api/v1/network/closeSoftAp", {})
            time.sleep(2)
        except Exception:
            pass
    print(f"baseline: ip={info.get('ip')} mqtt={info.get('mqttConnected')} softap={info.get('softapUp')} panelOk={info.get('panelOk')}")

    t0 = time.time()
    case_web_root(api, st)
    case_info_spam(api, st, n=12)
    case_devinfo_rename(api, st)

    mq0 = api.get("/api/v1/network/getMqttParam")
    restore_mqtt = {
        "host": mq0.get("host") or "",
        "port": int(mq0.get("port") or 8883),
        "transport": mq0.get("transport") or "mqtt",
        "qos": int(mq0.get("qos") or 1),
        "username": mq0.get("username") or "",
        "password": mq0.get("password") or "",
        "clientId": mq0.get("clientId") or "",
    }
    case_mqtt_bad_payloads(api, st, restore_mqtt)
    case_power_param_safe(api, st)
    case_wifi_scan(api, st, rounds=max(2, args.rounds // 2))
    case_mqtt_boundary(api, st, rounds=max(2, args.rounds // 2))
    case_mqtt_downlink(api, st, watch, {**restore_mqtt, **mq0}, cli_password=args.mqtt_password)
    # upload after downlink drain, before render-busy flood (needs panel lock)
    if not args.skip_upload:
        case_upload_frame(api, st, watch)
    case_display_bad(api, st)
    case_display(api, st, rounds=max(2, args.rounds // 2))
    case_render_busy(api, st)
    case_idempotent_softap(api, st, watch)
    case_softap_plus_display(api, st, watch)
    case_softap_scan_info(api, st, watch)
    case_softap_cycle(api, st, watch, rounds=args.rounds)
    if not args.skip_wifi_wrong:
        case_wifi_wrong_pass_restore(api, st)

    # final health
    print("\n[case] final health")
    try:
        fin = wait_sta(api, 25)
        mq = api.get("/api/v1/network/getMqttParam")
        if fin.get("wifiConnected") and not fin.get("mqttConnected"):
            api.post("/api/v1/network/setMqttParam", restore_mqtt)
            time.sleep(1.5)
            api.post("/api/v1/network/testMqtt", {})
            time.sleep(3)
            fin = api.get("/api/v1/system/getDevInfo")
        # ensure always-on
        api.post("/api/v1/system/setPowerParam", {"mode": "always", "sleepMin": 0, "wakeUrl": ""})
        st.mark(
            "final",
            bool(fin.get("wifiConnected")) and not fin.get("softapUp") and len(watch.panics) == 0,
            f"wifi={fin.get('wifiConnected')} mqtt={fin.get('mqttConnected')} softap={fin.get('softapUp')} "
            f"panics={len(watch.panics)} reboots~={watch.reboots} mqttHost={mq.get('host')}",
        )
    except Exception as e:
        st.mark("final", False, str(e))

    elapsed = time.time() - t0
    watch.stop()

    print("\n======== SUMMARY ========")
    print(f"passed={st.ok} failed={st.fail} elapsed={elapsed:.1f}s serial_panics={len(watch.panics)} reboot_events~={watch.reboots}")
    if watch.panics:
        print("panic lines:")
        for line in watch.panics[-10:]:
            print(" ", line)
    if st.notes:
        print("failures:")
        for n in st.notes:
            print(" ", n)
    return 0 if st.fail == 0 and not watch.panics else 1


if __name__ == "__main__":
    sys.exit(main())
