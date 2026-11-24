#!/usr/bin/env python3
"""EK410 single-image release smoke (SoftAP XOR BLE + MQTT stop recovery).

Prerequisites:
  - Current ek410.bin flashed to the board
  - USB serial + STA Wi-Fi (IP discovered from log if --host omitted)

Steps:
  1. Hard reset, discover STA IP, getDevInfo (projectName=ek410)
  2. Event-log meta + SoftAP CT-Prov read-only suite
  3. SoftAP → BLE → SoftAP idle round-trip (in-process XOR)
  4. Race: testMqtt / telemetry vs openBleProv — accept BLE up OR
     mqtt-busy abort with STA/httpd restored; then retry a clean XOR

Usage:
  python3 tools/release_smoke.py --port /dev/cu.wchusbserial3130
  python3 tools/release_smoke.py --host 192.168.31.42 --port /dev/cu.wchusbserial3130
"""
from __future__ import annotations

import argparse
import json
import re
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(Path(__file__).resolve().parent))

try:
    import serial
except ImportError:
    print("FAIL: pyserial required (pip install pyserial)")
    sys.exit(2)

import ct_prov_softap_test as softap_suite  # noqa: E402


def http_json(method: str, url: str, body: dict | None = None, timeout: float = 12.0):
    data = None
    headers = {}
    if body is not None:
        data = json.dumps(body).encode("utf-8")
        headers["Content-Type"] = "application/json"
    req = urllib.request.Request(url, data=data, headers=headers, method=method)
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        raw = resp.read()
        ctype = resp.headers.get("Content-Type", "")
        if "json" in ctype or raw[:1] in (b"{", b"["):
            return resp.status, json.loads(raw.decode("utf-8"))
        return resp.status, raw


def http_json_allow_fail(method: str, url: str, body: dict | None = None, timeout: float = 12.0):
    try:
        return http_json(method, url, body, timeout)
    except urllib.error.HTTPError as e:
        raw = e.read() if e.fp else b""
        try:
            return e.code, json.loads(raw.decode("utf-8"))
        except Exception:
            return e.code, {"msg": raw.decode("utf-8", "replace")}
    except Exception as e:
        return -1, {"msg": str(e)}


def read_until(ser: serial.Serial, patterns: list[str], timeout: float, echo: bool = True) -> str:
    deadline = time.time() + timeout
    buf = ""
    while time.time() < deadline:
        chunk = ser.read(4096)
        if chunk:
            t = chunk.decode("utf-8", "replace")
            if echo:
                sys.stdout.write(t)
                sys.stdout.flush()
            buf += t
            for p in patterns:
                if p in buf:
                    return buf
        else:
            time.sleep(0.05)
    raise TimeoutError(f"timeout waiting for {patterns!r}")


def hard_reset(ser: serial.Serial) -> None:
    ser.setDTR(False)
    ser.setRTS(True)
    time.sleep(0.12)
    ser.setRTS(False)


def discover_ip(ser: serial.Serial, timeout: float = 55.0) -> str:
    buf = read_until(ser, ["got ip ", "web server up"], timeout)
    m = re.search(r"got ip (\d+\.\d+\.\d+\.\d+)", buf)
    if not m:
        deadline = time.time() + 25
        while time.time() < deadline and not m:
            chunk = ser.read(4096)
            if chunk:
                t = chunk.decode("utf-8", "replace")
                sys.stdout.write(t)
                sys.stdout.flush()
                buf += t
                m = re.search(r"got ip (\d+\.\d+\.\d+\.\d+)", buf)
        if not m:
            raise RuntimeError("STA IP not found in serial log")
    return m.group(1)


def wait_devinfo(host: str, timeout: float = 45.0) -> dict:
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        try:
            st, info = http_json("GET", f"http://{host}/api/v1/system/getDevInfo", timeout=5)
            assert st == 200 and isinstance(info, dict)
            return info
        except Exception as e:
            last = e
            time.sleep(1.5)
    raise RuntimeError(f"getDevInfo failed: {last}")


def wait_http_ready(host: str, timeout: float = 90.0) -> dict:
    """Poll until getDevInfo answers after BLE/XOR or reboot churn."""
    return wait_devinfo(host, timeout=timeout)


def open_ble(host: str, idle_sec: int) -> None:
    st, body = http_json_allow_fail(
        "POST",
        f"http://{host}/api/v1/network/openBleProv",
        {"idleSec": idle_sec},
        timeout=8,
    )
    print(f"openBleProv HTTP status={st} body={body}", flush=True)


def wait_ble_or_abort(ser: serial.Serial, timeout: float) -> str:
    """Return 'ble' | 'abort' based on serial markers."""
    deadline = time.time() + timeout
    buf = ""
    ble_marks = ("NimBLE CT-Prov started", "BLE CT-Prov up")
    abort_marks = ("enter aborted", "mqtt_stop busy", "BLE enter aborted")
    while time.time() < deadline:
        chunk = ser.read(4096)
        if chunk:
            t = chunk.decode("utf-8", "replace")
            sys.stdout.write(t)
            sys.stdout.flush()
            buf += t
            if "CT-Prov BLE start failed" in buf:
                raise RuntimeError("NimBLE start failed")
            if "ek410_ble" in buf:
                raise RuntimeError("unexpected dual-SKU ek410_ble image")
            if any(m in buf for m in ble_marks):
                return "ble"
            if any(m in buf for m in abort_marks):
                return "abort"
        else:
            time.sleep(0.05)
    raise TimeoutError(f"no BLE-up or mqtt-abort within {timeout}s")


def wait_ble_exit(ser: serial.Serial, idle_sec: int) -> str:
    buf = read_until(
        ser,
        ["BLE CT-Prov stopped", "web server up", "BLE exit"],
        timeout=idle_sec + 90,
    )
    m = re.search(r"got ip (\d+\.\d+\.\d+\.\d+)", buf)
    if m:
        return m.group(1)
    # httpd often returns before STA has an IP again — keep reading.
    try:
        more = read_until(ser, ["got ip "], timeout=90, echo=True)
        buf += more
    except TimeoutError:
        pass
    m = re.search(r"got ip (\d+\.\d+\.\d+\.\d+)", buf)
    return m.group(1) if m else ""


def main() -> int:
    ap = argparse.ArgumentParser(description="EK410 release smoke (single image)")
    ap.add_argument("--port", default="/dev/cu.wchusbserial3130")
    ap.add_argument("--host", default="", help="STA IP if known")
    ap.add_argument("--idle-sec", type=int, default=20)
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--skip-reset", action="store_true")
    ap.add_argument(
        "--mqtt-password",
        default="",
        help="MQTT password for steps that change the broker and must restore it "
             "(getMqttParam only reports hasPassword)",
    )
    args = ap.parse_args()

    failures = 0

    def check(name: str, fn) -> None:
        nonlocal failures
        try:
            fn()
            print(f"\nPASS {name}\n", flush=True)
        except Exception as e:
            failures += 1
            print(f"\nFAIL {name}: {e}\n", flush=True)

    ser = serial.Serial(args.port, args.baud, timeout=0.2)
    host = args.host

    def step_boot():
        nonlocal host
        if not args.skip_reset:
            hard_reset(ser)
            # Prefer known host: STA may reconnect after a failed first associate
            # without reprinting "got ip" in time for a short serial window.
            if host:
                deadline = time.time() + 90
                while time.time() < deadline:
                    chunk = ser.read(4096)
                    if chunk:
                        sys.stdout.write(chunk.decode("utf-8", "replace"))
                        sys.stdout.flush()
                    try:
                        info = wait_devinfo(host, timeout=3)
                        break
                    except Exception:
                        time.sleep(1)
                else:
                    raise RuntimeError(f"getDevInfo timeout after reset ({host})")
            else:
                host = discover_ip(ser, 120)
        elif not host:
            host = discover_ip(ser, 60)
        info = wait_devinfo(host, 30)
        assert info.get("projectName") == "ek410", info
        assert info.get("bleSkuAvailable") is True, info
        assert info.get("wifiConnected") is True, info
        print("getDevInfo", json.dumps({
            k: info.get(k) for k in (
                "projectName", "fwVersion", "ip", "wifiConnected", "mqttConnected",
                "softapUp", "bleProvUp", "evtLogPresent", "evtLogBytes", "bleSkuAvailable",
            )
        }))
        # Keep host in sync with device-reported IP when possible
        if info.get("ip"):
            host = str(info["ip"])

    def step_evtlog_meta():
        st, meta = http_json("GET", f"http://{host}/api/v1/system/getEventLogMeta")
        assert st == 200 and isinstance(meta, dict), meta
        print("eventLogMeta", meta)
        # New partition table → present; old OTA-only table → present=false (still OK)
        assert "present" in meta and "capBytes" in meta

    def step_ctprov_suite():
        rc = softap_suite.run(host)
        assert rc == 0, f"ct_prov_softap_test failures rc={rc}"

    def step_xor_roundtrip():
        nonlocal host
        open_ble(host, args.idle_sec)
        outcome = wait_ble_or_abort(ser, 70)
        assert outcome == "ble", f"expected BLE up, got {outcome}"
        new_ip = wait_ble_exit(ser, args.idle_sec)
        if new_ip:
            host = new_ip
        # GOT_IP may land before httpd (retry timer) — poll until port 80 answers.
        deadline = time.time() + 120
        info = None
        last = None
        while time.time() < deadline:
            try:
                info = wait_devinfo(host, timeout=5)
                assert info.get("projectName") == "ek410"
                assert info.get("bleProvUp") is False
                assert info.get("wifiConnected") is True, info
                break
            except Exception as e:
                last = e
                time.sleep(2)
        if info is None:
            raise RuntimeError(f"getDevInfo after BLE exit failed: {last}")
        if info.get("ip"):
            host = str(info["ip"])
        print("after XOR", info.get("ip"), "mqtt=", info.get("mqttConnected"))
        # Post-BLE Wi-Fi/MQTT need a few seconds to settle (heap coalesce).
        time.sleep(5)

    def step_mqtt_busy_then_retry_xor():
        nonlocal host
        import threading

        info = wait_devinfo(host, 60)
        assert info.get("wifiConnected") is True, info
        # Wait until MQTT is up so the race can actually hold the client mutex.
        deadline = time.time() + 60
        while time.time() < deadline and not info.get("mqttConnected"):
            time.sleep(2)
            info = wait_devinfo(host, 10)
        print("pre-race mqttConnected=", info.get("mqttConnected"), flush=True)

        # Overlap long testMqtt (holds MQTT mutex during publish) with openBleProv.
        print("race: background testMqtt + openBleProv", flush=True)

        def hammer():
            http_json_allow_fail(
                "POST", f"http://{host}/api/v1/network/testMqtt", {}, timeout=12
            )

        thr = threading.Thread(target=hammer, daemon=True)
        thr.start()
        time.sleep(0.15)
        open_ble(host, args.idle_sec)
        outcome = wait_ble_or_abort(ser, 75)
        thr.join(timeout=15)
        print(f"race outcome={outcome}", flush=True)

        if outcome == "abort":
            # Original mode must remain usable; telemetry path must not be dead.
            info = wait_devinfo(host, 40)
            assert info.get("bleProvUp") is False, info
            assert info.get("projectName") == "ek410", info
            # SoftAP may or may not be restored; STA httpd must answer.
            st, meta = http_json("GET", f"http://{host}/api/v1/system/getEventLogMeta")
            assert st == 200, meta
            # Give MQTT a beat, then verify probe still works (periodic path alive).
            time.sleep(2)
            st2, body = http_json_allow_fail(
                "POST", f"http://{host}/api/v1/network/testMqtt", {}, timeout=20
            )
            print("post-abort testMqtt", st2, body, flush=True)
            assert st2 == 200, body
            print("mqtt-busy abort recovered — retry clean XOR", flush=True)
        else:
            # Race lost: BLE started — wait idle exit then continue.
            new_ip = wait_ble_exit(ser, args.idle_sec)
            if new_ip:
                host = new_ip
            wait_devinfo(host, 50)

        # Clean SoftAP→BLE→SoftAP must succeed on the same image.
        time.sleep(1)
        open_ble(host, args.idle_sec)
        outcome2 = wait_ble_or_abort(ser, 70)
        assert outcome2 == "ble", f"clean XOR retry expected BLE, got {outcome2}"
        new_ip = wait_ble_exit(ser, args.idle_sec)
        if new_ip:
            host = new_ip
        info = wait_devinfo(host, 50)
        assert info.get("bleProvUp") is False
        assert info.get("projectName") == "ek410"
        print("clean XOR after race OK", info.get("ip"))

    def step_mqtt_broker_change_consistency():
        """Busy broker change must not persist the new host (HTTP + CT-Prov)."""
        import threading

        wait_http_ready(host, 60)
        st, cur = http_json("GET", f"http://{host}/api/v1/network/getMqttParam", timeout=10)
        assert st == 200 and isinstance(cur, dict), cur
        old_host = cur.get("host") or ""
        if not old_host:
            print("skip broker-change — no mqtt host configured")
            return
        old_port = int(cur.get("port") or 1883)
        old_transport = cur.get("transport") or ("mqtts" if cur.get("tls") else "mqtt")
        # getMqttParam is write-only for the password (hasPassword), and a
        # broker/username change clears the stored one: this step can only
        # restore the device when the password is known.
        mqtt_password = args.mqtt_password or cur.get("password") or ""
        if cur.get("hasPassword") and not mqtt_password:
            print("skip broker-change — stored MQTT password is write-only; pass --mqtt-password to run")
            return
        fake = "broker-must-not-persist.invalid"
        busy_msgs = ("mqtt stop busy", "mqtt restart busy")

        # Hold MQTT mutex via overlapping testMqtt publishes, then change broker.
        stop_hammer = threading.Event()

        def hammer_telemetry():
            while not stop_hammer.is_set():
                http_json_allow_fail(
                    "POST", f"http://{host}/api/v1/network/testMqtt", {}, timeout=20
                )

        thr = threading.Thread(target=hammer_telemetry, daemon=True)
        thr.start()
        time.sleep(0.3)
        body_busy = {
            "host": fake,
            "port": old_port,
            "transport": old_transport,
            "username": cur.get("username") or "",
            "password": mqtt_password,
        }
        st_b, j_b = http_json_allow_fail(
            "POST", f"http://{host}/api/v1/network/setMqttParam", body_busy, timeout=25
        )
        stop_hammer.set()
        thr.join(timeout=25)
        wait_http_ready(host, 60)
        st_r, after = http_json("GET", f"http://{host}/api/v1/network/getMqttParam", timeout=10)
        assert st_r == 200, after
        got = after.get("host") or ""
        http_busy = isinstance(j_b, dict) and any(
            m in str(j_b.get("msg", "")) for m in busy_msgs
        )
        http_ok = st_b == 200 and isinstance(j_b, dict) and j_b.get("code") == 0
        print("http vs telemetry", st_b, j_b, "host=", got, "busy=", http_busy, flush=True)
        if http_busy:
            assert got == old_host, f"HTTP busy but NVS host={got!r}"
        elif http_ok:
            assert got == fake, after
        else:
            raise AssertionError(f"unexpected setMqttParam: {st_b} {j_b}")

        # Restore known-good broker via CT-Prov; if busy, retry once.
        restore = {
            "enable": True,
            "host": old_host,
            "port": old_port,
            "tls": old_transport == "mqtts",
        }
        if cur.get("username"):
            restore["username"] = cur["username"]
        if mqtt_password:
            restore["password"] = mqtt_password
        for attempt in range(3):
            st3, j3, _ = softap_suite.post_ctprov(
                host,
                {"id": 93, "op": "config.set", "data": {"mqtt": restore}},
                timeout=25,
            )
            assert st3 == 200 and j3 is not None, j3
            if j3.get("ok"):
                break
            code = (j3.get("error") or {}).get("code")
            assert code == "RESOURCE_BUSY", j3
            time.sleep(1.5)
        else:
            raise AssertionError(f"restore still busy: {j3}")

        # CT-Prov busy race: overlapping config.set with two fake hosts.
        fake_c = "ctprov-must-not-persist.invalid"
        results = [None, None]

        def set_ct(idx, hname):
            payload = {
                "enable": True,
                "host": hname,
                "port": old_port,
                "tls": old_transport == "mqtts",
            }
            stc, jc, _ = softap_suite.post_ctprov(
                host,
                {"id": 94 + idx, "op": "config.set", "data": {"mqtt": payload}},
                timeout=25,
            )
            results[idx] = (stc, jc)

        th1 = threading.Thread(target=set_ct, args=(0, fake_c), daemon=True)
        th2 = threading.Thread(target=set_ct, args=(1, fake_c + ".b"), daemon=True)
        th1.start()
        time.sleep(0.05)
        th2.start()
        th1.join(timeout=30)
        th2.join(timeout=30)
        print("ctprov race", results, flush=True)
        st_g, jg, _ = softap_suite.post_ctprov(
            host, {"id": 96, "op": "config.get", "data": {}}, timeout=15
        )
        assert st_g == 200 and jg and jg.get("ok"), jg
        got2 = ((jg.get("data") or {}).get("mqtt") or {}).get("host") or ""
        for stc, jc in results:
            if jc and jc.get("ok") is False:
                assert (jc.get("error") or {}).get("code") == "RESOURCE_BUSY", jc
                # Busy response must not have been the sole writer of got2
        # If both busy → still old_host (or whatever restore left).
        both_busy = all(
            jc and jc.get("ok") is False for _, jc in results if jc is not None
        )
        if both_busy:
            assert got2 == old_host, f"CT-Prov both busy but host={got2!r}"
        print("after CT-Prov race host=", got2)

        # Final restore + readback
        for attempt in range(3):
            st4, j4, _ = softap_suite.post_ctprov(
                host,
                {"id": 97, "op": "config.set", "data": {"mqtt": restore}},
                timeout=25,
            )
            if j4 and j4.get("ok"):
                break
            time.sleep(1.5)
        st5, final = http_json("GET", f"http://{host}/api/v1/network/getMqttParam")
        assert st5 == 200 and final.get("host") == old_host, final
        print("mqtt broker-change consistency OK")

    def step_mqtt_ca_persist_reboot():
        """Save CA via Web, reboot, confirm NVS readback; then restore."""
        nonlocal host
        wait_http_ready(host, 60)
        st, cur = http_json("GET", f"http://{host}/api/v1/network/getMqttParam", timeout=10)
        assert st == 200 and isinstance(cur, dict), cur
        if not cur.get("host"):
            print("skip CA persist — no mqtt host")
            return
        old_ca = cur.get("caPem") or ""
        marker = "# EK410-CA-SMOKE-MARKER\n"
        body = {
            "host": cur["host"],
            "port": int(cur.get("port") or 1883),
            "transport": cur.get("transport") or (
                "mqtts" if cur.get("tls") else "mqtt"
            ),
            "tlsVerify": bool(cur.get("tlsVerify", True)),
            "username": cur.get("username") or "",
            "password": cur.get("password") or "",
            "qos": int(cur.get("qos") or 1),
            "caPem": marker,
        }
        st2, j2 = http_json_allow_fail(
            "POST", f"http://{host}/api/v1/network/setMqttParam", body, timeout=25
        )
        assert st2 == 200 and isinstance(j2, dict) and j2.get("code") == 0, j2
        st3, mid = http_json("GET", f"http://{host}/api/v1/network/getMqttParam")
        assert st3 == 200 and (mid.get("caPem") or "") == marker, mid

        print("reboot to verify CA NVS…", flush=True)
        http_json_allow_fail(
            "POST", f"http://{host}/api/v1/system/setDevReset", {}, timeout=5
        )
        # Boot paint alone is ~40–50s before Wi-Fi; allow a long window.
        time.sleep(5)
        try:
            # Serial often prints got-ip before HTTP answers.
            new_ip = discover_ip(ser, timeout=150)
            if new_ip:
                host = new_ip
                print("serial got ip", host, flush=True)
        except Exception as e:
            print("serial discover after reboot:", e, flush=True)
        deadline = time.time() + 120
        info = None
        while time.time() < deadline:
            try:
                info = wait_devinfo(host, timeout=5)
                if info.get("ip"):
                    host = str(info["ip"])
                break
            except Exception:
                time.sleep(2)
        if info is None:
            raise RuntimeError("device did not return after CA reboot")

        st4, after = http_json("GET", f"http://{host}/api/v1/network/getMqttParam")
        assert st4 == 200, after
        assert (after.get("caPem") or "") == marker, after
        print("CA survived reboot OK")

        body["caPem"] = old_ca
        for _ in range(3):
            st5, j5 = http_json_allow_fail(
                "POST", f"http://{host}/api/v1/network/setMqttParam", body, timeout=25
            )
            if st5 == 200 and isinstance(j5, dict) and j5.get("code") == 0:
                break
            time.sleep(1.5)
        else:
            raise AssertionError(f"restore CA failed: {st5} {j5}")

    def step_mqtt_disable_busy_status():
        """Disable MQTT while idle should succeed; response shape validated."""
        st, j, _ = softap_suite.post_ctprov(
            host,
            {"id": 90, "op": "config.get", "data": {}},
        )
        assert st == 200 and j and j.get("ok"), j
        mqtt = (j.get("data") or {}).get("mqtt") or {}
        host_name = mqtt.get("host") or ""
        if not host_name:
            print("skip disable/reenable — no mqtt host configured")
            return
        # Disable
        st2, j2, _ = softap_suite.post_ctprov(
            host,
            {"id": 91, "op": "config.set", "data": {"mqtt": {"enable": False}}},
            timeout=20,
        )
        assert st2 == 200 and j2 is not None, j2
        if j2.get("ok") is False:
            # Busy is acceptable under load; must be RESOURCE_BUSY
            code = (j2.get("error") or {}).get("code")
            assert code == "RESOURCE_BUSY", j2
            print("disable busy (acceptable under load)", j2)
            return
        assert j2.get("ok") is True, j2
        # Re-enable with prior host (minimal fields)
        payload = {
            "enable": True,
            "host": host_name,
            "port": mqtt.get("port") or 1883,
            "tls": bool(mqtt.get("tls")),
        }
        if mqtt.get("username"):
            payload["username"] = mqtt["username"]
        st3, j3, _ = softap_suite.post_ctprov(
            host,
            {"id": 92, "op": "config.set", "data": {"mqtt": payload}},
            timeout=20,
        )
        assert st3 == 200 and j3 is not None, j3
        if j3.get("ok") is False:
            code = (j3.get("error") or {}).get("code")
            assert code in ("RESOURCE_BUSY", "INVALID_ARGUMENT"), j3
            print("re-enable reported", code, j3)
        else:
            assert j3.get("ok") is True, j3
            print("mqtt disable/re-enable OK")

    check("boot_getDevInfo", step_boot)
    if failures:
        ser.close()
        return 1
    check("event_log_meta", step_evtlog_meta)
    check("ctprov_softap_suite", step_ctprov_suite)
    check("softap_ble_xor", step_xor_roundtrip)
    check("mqtt_busy_race_then_xor", step_mqtt_busy_then_retry_xor)
    check("mqtt_broker_change_consistency", step_mqtt_broker_change_consistency)
    check("mqtt_ca_persist_reboot", step_mqtt_ca_persist_reboot)
    check("mqtt_disable_status", step_mqtt_disable_busy_status)

    ser.close()
    print(f"\n=== release smoke done: {failures} failure(s) ===")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
