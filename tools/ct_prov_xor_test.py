#!/usr/bin/env python3
"""SoftAP ↔ BLE in-process XOR smoke test for EK410 (single image).

Prerequisites:
  - One ek410.bin flashed (tools/flash_release.sh)
  - Device on Wi-Fi with known STA IP (or discover via serial)

Flow:
  1) getDevInfo (projectName=ek410, bleSkuAvailable=true)
  2) SoftAP CT-Prov device.info.get
  3) openBleProv with short idleSec → NimBLE up in-process (no reboot / no ek410_ble)
  4) Serial: BLE CT-Prov up, then idle exit → httpd restored
  5) getDevInfo again on same image

Usage:
  python3 tools/ct_prov_xor_test.py --port /dev/cu.wchusbserial3130
  python3 tools/ct_prov_xor_test.py --host 192.168.31.42 --port /dev/cu.wchusbserial3130
"""
from __future__ import annotations

import argparse
import json
import re
import sys
import time
import urllib.error
import urllib.request

try:
    import serial
except ImportError:
    print("FAIL: pyserial required (pip install pyserial)")
    sys.exit(2)


def http_json(method: str, url: str, body: dict | None = None, timeout: float = 10.0):
    data = None
    headers = {}
    if body is not None:
        data = json.dumps(body).encode("utf-8")
        headers["Content-Type"] = "application/json"
    req = urllib.request.Request(url, data=data, headers=headers, method=method)
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        raw = resp.read()
        return resp.status, json.loads(raw.decode("utf-8"))


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


def discover_ip(ser: serial.Serial, timeout: float = 45.0) -> str:
    buf = read_until(ser, ["got ip ", "web server up"], timeout)
    m = re.search(r"got ip (\d+\.\d+\.\d+\.\d+)", buf)
    if not m:
        deadline = time.time() + 20
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


def main() -> int:
    ap = argparse.ArgumentParser(description="EK410 SoftAP↔BLE XOR smoke (single image)")
    ap.add_argument("--port", default="/dev/cu.wchusbserial3130")
    ap.add_argument("--host", default="", help="STA IP if already known")
    ap.add_argument("--idle-sec", type=int, default=25, help="BLE window auto-exit for CI")
    ap.add_argument("--baud", type=int, default=115200)
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

    def step_boot_httpd():
        nonlocal host
        hard_reset(ser)
        if not host:
            host = discover_ip(ser, 50)
        else:
            read_until(ser, ["web server up", "running"], 40)
        deadline = time.time() + 20
        last_err = None
        while time.time() < deadline:
            try:
                st, info = http_json("GET", f"http://{host}/api/v1/system/getDevInfo")
                assert st == 200
                assert info.get("projectName") == "ek410", info
                assert info.get("bleSkuAvailable") is True, info
                print("getDevInfo", json.dumps({k: info.get(k) for k in
                      ("projectName", "bleSkuAvailable", "ip", "softapUp", "bleProvUp")}))
                return
            except Exception as e:
                last_err = e
                time.sleep(1)
        raise RuntimeError(f"getDevInfo failed: {last_err}")

    def step_ctprov_info():
        body = {"id": 1, "op": "device.info.get", "data": {}}
        req = urllib.request.Request(
            f"http://{host}/api/v1/ctprov",
            data=json.dumps(body).encode(),
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        with urllib.request.urlopen(req, timeout=10) as resp:
            raw = json.loads(resp.read().decode())
        assert raw.get("op") == "device.info.get.result" or "result" in str(raw.get("op", "")), raw
        print("ctprov", raw)

    def step_ble_xor_roundtrip():
        nonlocal host
        print(f"openBleProv idleSec={args.idle_sec}", flush=True)
        try:
            http_json("POST", f"http://{host}/api/v1/network/openBleProv",
                      {"idleSec": args.idle_sec}, timeout=8)
        except Exception as e:
            # httpd may stop before the response finishes (expected on XOR)
            print(f"openBleProv HTTP note: {e}", flush=True)

        buf = read_until(
            ser,
            ["NimBLE CT-Prov started", "BLE CT-Prov up"],
            timeout=70,
        )
        if "CT-Prov BLE start failed" in buf:
            raise RuntimeError("NimBLE failed to start (in-process XOR)")
        if "ek410_ble" in buf:
            raise RuntimeError("unexpected ek410_ble project name — dual-SKU image?")

        # Idle exit restores httpd on the same image (no SoftAP SKU reboot)
        buf2 = read_until(
            ser,
            ["BLE CT-Prov stopped", "web server up", "BLE exit"],
            timeout=args.idle_sec + 90,
        )
        restored = (
            "web server up" in buf2
            or "BLE CT-Prov stopped" in buf2
            or "BLE exit" in buf2
        )
        if not restored:
            try:
                buf2 += read_until(ser, ["web server up", "BLE CT-Prov stopped"], timeout=40)
                restored = True
            except TimeoutError:
                pass
        assert restored, "did not leave BLE / restore httpd"

        m = re.search(r"got ip (\d+\.\d+\.\d+\.\d+)", buf2)
        if m:
            host = m.group(1)
        deadline = time.time() + 45
        last = None
        while time.time() < deadline:
            try:
                st, info = http_json("GET", f"http://{host}/api/v1/system/getDevInfo")
                assert info.get("projectName") == "ek410"
                assert info.get("bleSkuAvailable") is True
                assert info.get("bleProvUp") is False
                print("back after BLE XOR", info.get("projectName"), info.get("ip"),
                      "bleProvUp=", info.get("bleProvUp"))
                return
            except Exception as e:
                last = e
                time.sleep(1)
        raise RuntimeError(f"getDevInfo after BLE exit failed: {last}")

    check("boot_httpd", step_boot_httpd)
    if failures:
        ser.close()
        return 1
    check("ctprov_device_info", step_ctprov_info)
    check("ble_xor_roundtrip", step_ble_xor_roundtrip)

    ser.close()
    print(f"\n=== single-image XOR test done: {failures} failure(s) ===")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
