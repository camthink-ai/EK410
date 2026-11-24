#!/usr/bin/env python3
"""On-device verification of the 1.3.10 fixes — runs against a real EK410.

    python3 tools/fix_verification.py --host 192.168.1.50
    python3 tools/fix_verification.py --host 192.168.1.50 --url http://192.168.1.10/t.png
    python3 tools/fix_verification.py --host 192.168.1.50 --firmware dist/ek410.bin
    python3 tools/fix_verification.py --host 192.168.1.50 --power        # bench unit only

Only the device's HTTP API is used (LAN or SoftAP), nothing else. Every check
maps to one entry of CHANGELOG 1.3.10 and FAILS on 1.3.9 firmware:

  identity              device answers, is an EK410, runs the expected version
  json_depth_guard      300 nested '[' on 4 route families must not reboot it
  dev_name_limit        a 4000-byte devName is rejected, the name is kept
  mqtt_password_hidden  getMqttParam has no "password" (hasPassword instead)
  ota_failure_visible   a failing URL OTA reports otaState=failed + otaError
  url_display_soak      --url: N URL displays; no reboot, PSRAM must recover
  ota_roundtrip         --firmware: local upload, reboot into the other slot
  sleep_min_clamp       --power: sleepMin is clamped (saves power config twice)

Exit 0 only when every executed check passed; a check that lacks its option or
device state is reported SKIPPED, never counted as a pass. Reboots are detected
through getDevInfo.uptime_s, which must never go backwards.
"""
from __future__ import annotations

import argparse
import http.client
import json
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from qa_lib.firmware import firmware_app_version  # noqa: E402

SLEEP_MIN_MAX = 525600
LOW_STACK = 300   # bytes: warn
MIN_STACK = 128   # bytes: fail


class Device:
    def __init__(self, host: str, timeout: float = 10.0) -> None:
        self.host = host
        self.timeout = timeout
        self.last_uptime: int | None = None

    def call(self, method: str, path: str, body: bytes | None = None, timeout: float | None = None):
        req = urllib.request.Request("http://" + self.host + path, data=body, method=method)
        if body is not None:
            req.add_header("Content-Type", "application/json")
        try:
            with urllib.request.urlopen(req, timeout=timeout or self.timeout) as r:
                return r.status, r.read()
        except urllib.error.HTTPError as e:
            return e.code, e.read()
        except (urllib.error.URLError, OSError) as e:
            return None, str(e).encode()

    def json(self, method: str, path: str, obj=None, timeout: float | None = None):
        status, data = self.call(method, path, None if obj is None else json.dumps(obj).encode(), timeout)
        try:
            parsed = json.loads(data.decode("utf-8", "replace")) if status is not None else None
        except ValueError:
            parsed = None
        return status, parsed

    def info(self):
        return self.json("GET", "/api/v1/system/getDevInfo")

    def wait_info(self, seconds: float):
        """Poll until getDevInfo answers; the info dict, or None after `seconds`."""
        end = time.time() + seconds
        while True:
            status, info = self.info()
            if status == 200 and isinstance(info, dict) and "uptime_s" in info:
                return info
            if time.time() >= end:
                return None
            time.sleep(2)

    def uptime_ok(self, info) -> bool:
        """False when the device rebooted since the previous look."""
        up = int(info.get("uptime_s", -1))
        if self.last_uptime is not None and up < self.last_uptime:
            return False
        self.last_uptime = up
        return True


class Report:
    def __init__(self) -> None:
        self.rows: list[tuple[str, str, str]] = []
        self.stack_min: dict[str, int] = {}
        self.notes: list[str] = []

    def add(self, name: str, status: str, detail: str = "") -> None:
        self.rows.append((name, status, detail))
        print(f"  {status:6} {name:22} {detail}", flush=True)

    def track(self, info) -> None:
        for task, val in (info.get("stackMin") or {}).items():
            if isinstance(val, (int, float)):
                self.stack_min[task] = min(self.stack_min.get(task, 1 << 30), int(val))


def expect_version_default() -> str:
    try:
        return (ROOT / "VERSION").read_text(encoding="utf-8").strip()
    except OSError:
        return ""


def check_identity(dev: Device, rep: Report, expect: str):
    info = dev.wait_info(30)
    if info is None:
        rep.add("identity", "FAIL", "device did not answer getDevInfo in 30 s")
        return None
    problems = []
    if info.get("projectName") != "ek410":
        problems.append(f"projectName={info.get('projectName')!r}")
    if expect and info.get("fwVersion") != expect:
        problems.append(f"runs fwVersion={info.get('fwVersion')!r}, expected {expect!r}")
    heap = info.get("heap") or {}
    rep.add("identity", "FAIL" if problems else "PASS",
            (", ".join(problems) + " — " if problems else "") +
            f"fw={info.get('fwVersion')} part={info.get('otaPartition')} "
            f"heap free={heap.get('free')} min={heap.get('minFree')} largestInt={heap.get('largestInt')} "
            f"spiram={heap.get('freeSpiram')}/{heap.get('largestSpiram')}" if heap else
            ("; ".join(problems) or f"fw={info.get('fwVersion')}") + " (no heap diagnostics: firmware older than 1.3.10)")
    return info


def check_json_depth(dev: Device, rep: Report):
    routes = ["/api/v1/system/setDevInfo", "/api/v1/network/setMqttParam",
              "/api/v1/display/renderCanvas", "/api/v1/ctprov"]
    body = b"[" * 300
    bad = []
    for path in routes:
        _, before = dev.info()
        t0 = time.time()
        status, _ = dev.call("POST", path, body, timeout=20)
        after = dev.wait_info(60)
        took = time.time() - t0
        if status is None:
            bad.append(f"{path}: no answer")
        elif after is None:
            bad.append(f"{path}: device unreachable afterwards (rebooted and did not come back?)")
        elif not dev.uptime_ok(after):
            bad.append(f"{path}: uptime went {before.get('uptime_s')} -> {after.get('uptime_s')} (REBOOT)")
        elif took > 10:
            bad.append(f"{path}: took {took:.1f}s")
    rep.add("json_depth_guard", "FAIL" if bad else "PASS",
            "; ".join(bad) if bad else f"{len(routes)} route families answered, no reboot")


def check_dev_name(dev: Device, rep: Report):
    info = dev.wait_info(30)
    if info is None:
        rep.add("dev_name_limit", "FAIL", "device unreachable")
        return
    name = str(info.get("devName") or "")
    status, resp = dev.json("POST", "/api/v1/system/setDevInfo", {"devName": "A" * 4000})
    code = (resp or {}).get("code")
    after = dev.wait_info(30)
    dev.json("POST", "/api/v1/system/setDevInfo", {"devName": name})  # restore in any case
    if status != 200 or code == 0:
        rep.add("dev_name_limit", "FAIL", f"4000-byte name accepted (status={status} code={code})")
    elif after is None or not dev.uptime_ok(after):
        rep.add("dev_name_limit", "FAIL", "device rebooted on the long name (uptime reset)")
    elif str(after.get("devName")) != name:
        rep.add("dev_name_limit", "FAIL", f"name changed to {after.get('devName')!r}")
    else:
        rep.add("dev_name_limit", "PASS", f"rejected ({(resp or {}).get('msg')}), name kept")


def check_mqtt_hidden(dev: Device, rep: Report):
    status, m = dev.json("GET", "/api/v1/network/getMqttParam")
    if status != 200 or not isinstance(m, dict):
        rep.add("mqtt_password_hidden", "FAIL", f"getMqttParam status={status}")
    elif "password" in m:
        rep.add("mqtt_password_hidden", "FAIL", "getMqttParam still returns the password in clear")
    elif "hasPassword" not in m:
        rep.add("mqtt_password_hidden", "FAIL", "neither password nor hasPassword present")
    else:
        rep.add("mqtt_password_hidden", "PASS", f"hasPassword={m['hasPassword']}, no clear secret")


def check_ota_failure(dev: Device, rep: Report):
    before = dev.wait_info(30)
    if before is None or "otaState" not in before:
        rep.add("ota_failure_visible", "FAIL", "getDevInfo has no otaState (firmware older than 1.3.10?)")
        return
    was_up = bool(before.get("mqttConnected"))
    status, resp = dev.json("POST", "/api/v1/system/setDevUpgrade",
                            {"url": "https://127.0.0.1:9/ek410-missing.bin"}, timeout=20)
    if status != 200 or (resp or {}).get("code") != 0:
        rep.add("ota_failure_visible", "FAIL", f"setDevUpgrade rejected: status={status} {resp}")
        return
    state, err = None, ""
    t0 = time.time()
    end = t0 + 150
    while time.time() < end:
        info = dev.wait_info(10)
        if info is None:
            rep.add("ota_failure_visible", "FAIL", "device vanished during the failed OTA")
            return
        if not dev.uptime_ok(info):
            rep.add("ota_failure_visible", "FAIL", "device rebooted during the failed OTA")
            return
        state, err = info.get("otaState"), str(info.get("otaError") or "")
        if state == "failed":
            waited = time.time() - t0
            break
        time.sleep(2)
    if state != "failed" or not err:
        rep.add("ota_failure_visible", "FAIL", f"otaState={state!r} otaError={err!r} after 150 s")
        return
    if was_up:
        end = time.time() + 60
        while time.time() < end:
            info = dev.wait_info(10)
            if info and info.get("mqttConnected"):
                break
            time.sleep(3)
        else:
            rep.add("ota_failure_visible", "FAIL", f"failure reported ({err}) but MQTT did not come back in 60 s")
            return
    rep.add("ota_failure_visible", "PASS", f"otaState=failed, otaError={err!r} after {waited:.0f}s, device kept running")


def check_url_soak(dev: Device, rep: Report, url: str, rounds: int):
    spiram, display_s, failures = [], [], []
    for i in range(rounds):
        before = dev.wait_info(30)
        if before is None:
            failures.append(f"round {i + 1}: unreachable before")
            break
        status, resp = dev.json("POST", "/api/v1/display/displayUrl", {"url": url}, timeout=20)
        if status != 200 or (resp or {}).get("code") not in (0, None):
            failures.append(f"round {i + 1}: displayUrl status={status} {resp}")
            break
        rep.track(before)
        t0 = time.time()
        while True:
            info = dev.wait_info(15)
            if info is None:
                failures.append(f"round {i + 1}: device vanished mid-display")
                break
            if not dev.uptime_ok(info):
                failures.append(f"round {i + 1}: REBOOT mid-display (uptime reset)")
                break
            rep.track(info)
            if not info.get("displayBusy"):
                break
            if time.time() - t0 > 180:
                failures.append(f"round {i + 1}: displayBusy for >180 s")
                break
            time.sleep(2)
        if failures:
            break
        if info is None or not info.get("lastDisplayOk"):
            failures.append(f"round {i + 1}: display reported failure (lastDisplayOk={None if info is None else info.get('lastDisplayOk')})")
            break
        display_s.append(time.time() - t0)
        heap = info.get("heap") or {}
        if "largestSpiram" in heap:
            spiram.append(int(heap["largestSpiram"]))
    if failures:
        rep.add("url_display_soak", "FAIL", failures[0])
        return
    detail = f"{rounds} displays ok, {min(display_s):.0f}–{max(display_s):.0f} s each"
    if spiram:
        base = sorted(spiram[:3])[len(spiram[:3]) // 2]
        low = min(spiram)
        detail += f", largestSpiram {low}–{max(spiram)} (first {base})"
        if base and low < base * 0.6:
            rep.add("url_display_soak", "FAIL", detail + " — PSRAM largest block did not recover")
            return
    rep.add("url_display_soak", "PASS", detail)


def check_ota_roundtrip(dev: Device, rep: Report, image: Path):
    data = image.read_bytes()
    want = firmware_app_version(image)
    if not want:
        rep.add("ota_roundtrip", "FAIL", f"{image} is not an ESP app image")
        return
    before = dev.wait_info(30)
    if before is None:
        rep.add("ota_roundtrip", "FAIL", "device unreachable")
        return
    if before.get("softapUp"):
        rep.add("ota_roundtrip", "SKIPPED", "close the SoftAP first (upload over the hotspot is slow)")
        return
    conn = http.client.HTTPConnection(dev.host, timeout=600)
    try:
        conn.putrequest("POST", "/api/v1/system/uploadFirmware")
        conn.putheader("Content-Type", "application/octet-stream")
        conn.putheader("Content-Length", str(len(data)))
        conn.endheaders()
        for off in range(0, len(data), 32768):
            conn.send(data[off:off + 32768])
        resp = conn.getresponse()
        status, body = resp.status, resp.read()
    except OSError as e:
        rep.add("ota_roundtrip", "FAIL", f"upload broke: {e}")
        return
    finally:
        conn.close()
    try:
        accepted = status == 200 and json.loads(body.decode("utf-8", "replace")).get("code") == 0
    except (ValueError, AttributeError):
        accepted = False
    if not accepted:
        rep.add("ota_roundtrip", "FAIL", f"upload refused: status={status} {body[:120]!r}")
        return
    dev.last_uptime = None  # a reboot is the expected outcome here
    end = time.time() + 300
    after = None
    while time.time() < end:
        after = dev.wait_info(10)
        if after and after.get("otaPartition") != before.get("otaPartition"):
            break
        after = None
        time.sleep(3)
    if after is None:
        rep.add("ota_roundtrip", "FAIL", "device did not come back in the other slot within 300 s")
        return
    end = time.time() + 60
    while after.get("otaPendingVerify") and time.time() < end:
        time.sleep(3)
        after = dev.wait_info(10)
    problems = []
    if after.get("fwVersion") != want:
        problems.append(f"runs fwVersion={after.get('fwVersion')!r}, image is {want!r}")
    if after.get("otaPendingVerify"):
        problems.append("image stayed pending-verify (rollback gate never confirmed it)")
    rep.track(after)
    rep.add("ota_roundtrip", "FAIL" if problems else "PASS",
            "; ".join(problems) if problems else
            f"{before.get('otaPartition')} -> {after.get('otaPartition')}, fw={after.get('fwVersion')}, confirmed")


def check_sleep_min(dev: Device, rep: Report):
    status, p = dev.json("GET", "/api/v1/system/getPowerParam")
    if status != 200 or not isinstance(p, dict):
        rep.add("sleep_min_clamp", "FAIL", f"getPowerParam status={status}")
        return
    if p.get("mode") != "always":
        rep.add("sleep_min_clamp", "SKIPPED", f"unit is in mode {p.get('mode')!r} (needs always)")
        return
    dev.json("POST", "/api/v1/system/setPowerParam",
             {"mode": "cycle", "sleepMin": 2**31 - 1, "wakeSec": 600})
    _, clamped = dev.json("GET", "/api/v1/system/getPowerParam")
    dev.json("POST", "/api/v1/system/setPowerParam",
             {"mode": "always", "sleepMin": p.get("sleepMin", 0), "wakeSec": p.get("wakeSec", 120),
              "wakeUrl": p.get("wakeUrl", ""), "imageQuality": p.get("imageQuality", "quality")})
    _, back = dev.json("GET", "/api/v1/system/getPowerParam")
    if not clamped or not 1 <= int(clamped.get("sleepMin", 0)) <= SLEEP_MIN_MAX:
        rep.add("sleep_min_clamp", "FAIL", f"sleepMin clamped to {clamped and clamped.get('sleepMin')} (max {SLEEP_MIN_MAX})")
    elif not back or back.get("mode") != "always":
        rep.add("sleep_min_clamp", "FAIL", f"could not restore always mode (now {back and back.get('mode')!r})")
    else:
        rep.add("sleep_min_clamp", "PASS", f"clamped to {clamped.get('sleepMin')}, always mode restored")


def main() -> int:
    ap = argparse.ArgumentParser(description="On-device verification of the 1.3.10 fixes")
    ap.add_argument("--host", required=True, help="device IP (LAN or 192.168.4.1)")
    ap.add_argument("--expect-version", default=expect_version_default(),
                    help="fwVersion the device must run (default: repo VERSION; '' = any)")
    ap.add_argument("--url", default="", help="image URL the device can fetch, for the display soak")
    ap.add_argument("--rounds", type=int, default=20, help="URL displays in the soak")
    ap.add_argument("--firmware", type=Path, default=None,
                    help="app image for a local-upload OTA round trip (e.g. dist/ek410.bin)")
    ap.add_argument("--power", action="store_true",
                    help="also exercise the sleepMin clamp (saves the power config twice)")
    ap.add_argument("--json-out", type=Path, default=None)
    a = ap.parse_args()

    dev, rep = Device(a.host), Report()
    print(f"==> fix verification against {a.host}")
    info = check_identity(dev, rep, a.expect_version)
    if info is not None:
        rep.track(info)
        check_json_depth(dev, rep)
        check_dev_name(dev, rep)
        check_mqtt_hidden(dev, rep)
        check_ota_failure(dev, rep)
        if a.url:
            check_url_soak(dev, rep, a.url, a.rounds)
        else:
            rep.add("url_display_soak", "SKIPPED", "no --url")
        if a.firmware:
            check_ota_roundtrip(dev, rep, a.firmware)
        else:
            rep.add("ota_roundtrip", "SKIPPED", "no --firmware")
        if a.power:
            check_sleep_min(dev, rep)
        else:
            rep.add("sleep_min_clamp", "SKIPPED", "no --power")

    if rep.stack_min:
        low = ", ".join(f"{k}={v}" for k, v in sorted(rep.stack_min.items(), key=lambda kv: kv[1])[:6])
        print(f"  smallest stack headroom seen (bytes): {low}")
        for task, val in rep.stack_min.items():
            if val < MIN_STACK:
                rep.add(f"stack:{task}", "FAIL", f"only {val} bytes of stack left")
            elif val < LOW_STACK:
                rep.add(f"stack:{task}", "WARN", f"only {val} bytes of stack left")
    counts = {"PASS": 0, "FAIL": 0, "SKIPPED": 0, "WARN": 0}
    for _, st, _ in rep.rows:
        counts[st] = counts.get(st, 0) + 1
    print(f"==> {counts['PASS']} passed, {counts['FAIL']} failed, {counts['SKIPPED']} skipped"
          + (f", {counts['WARN']} warnings" if counts["WARN"] else ""))
    if a.json_out:
        a.json_out.parent.mkdir(parents=True, exist_ok=True)
        a.json_out.write_text(json.dumps(
            {"host": a.host, "ts": time.time(), "counts": counts,
             "stackMin": rep.stack_min,
             "checks": [{"name": n, "status": s, "detail": d} for n, s, d in rep.rows]},
            indent=2, ensure_ascii=False) + "\n")
    return 1 if counts["FAIL"] else 0


if __name__ == "__main__":
    sys.exit(main())
