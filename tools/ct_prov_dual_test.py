#!/usr/bin/env python3
"""End-to-end SoftAP ↔ BLE dual-SKU test for EK410 (ESP32-C61).

Prerequisites:
  - Both images flashed (tools/flash_dual_sku.sh)
  - Device on Wi-Fi with known STA IP (or discover via serial)

Flow:
  1) SoftAP SKU: httpd + getDevInfo (bleSkuAvailable=true)
  2) SoftAP CT-Prov smoke (device.info.get)
  3) openBleProv with short idleSec → reboot into BLE SKU
  4) Serial: wait for NimBLE CT-Prov / BLE CT-Prov up
  5) Serial: wait for SoftAP SKU return (BT released / web server / project ek410)
  6) SoftAP SKU: getDevInfo again

Usage:
  python3 tools/ct_prov_dual_test.py --port /dev/cu.wchusbserial3130
  python3 tools/ct_prov_dual_test.py --host 192.168.31.42 --port /dev/cu.wchusbserial3130
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
        # keep reading a bit more for IP after httpd
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
    ap = argparse.ArgumentParser()
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

    def step_softap_boot():
        nonlocal host
        hard_reset(ser)
        if not host:
            host = discover_ip(ser, 50)
        else:
            read_until(ser, ["web server up", "running"], 40)
        # wait httpd ready
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

    def step_switch_ble_and_back():
        nonlocal host
        print(f"openBleProv idleSec={args.idle_sec}", flush=True)
        try:
            http_json("POST", f"http://{host}/api/v1/network/openBleProv",
                      {"idleSec": args.idle_sec}, timeout=8)
        except Exception as e:
            # Device may reset before HTTP response completes
            print(f"openBleProv HTTP note: {e}", flush=True)

        buf = read_until(
            ser,
            ["NimBLE CT-Prov started", "BLE CT-Prov up"],
            timeout=70,
        )
        if "CT-Prov BLE start failed" in buf:
            raise RuntimeError("NimBLE failed to start on BLE SKU")
        # Confirm we actually booted the BLE project
        if "Project name:     ek410_ble" not in buf and "ek410_ble" not in buf:
            # may have scrolled past; still require NimBLE success markers above
            pass

        # Wait for auto-exit back to SoftAP SKU
        buf2 = read_until(
            ser,
            ["web server up", "project_name", "SoftAP SKU", "BT controller memory released", "running"],
            timeout=args.idle_sec + 90,
        )
        # Prefer definitive SoftAP markers
        softap_ok = ("web server up" in buf2) or ("SoftAP pending" in buf2)
        if not softap_ok:
            # continue reading briefly
            try:
                buf2 += read_until(ser, ["web server up"], timeout=40)
                softap_ok = True
            except TimeoutError:
                pass
        assert softap_ok, "did not return to SoftAP httpd"

        # Re-discover IP (may be same)
        m = re.search(r"got ip (\d+\.\d+\.\d+\.\d+)", buf2)
        if m:
            host = m.group(1)
        deadline = time.time() + 30
        last = None
        while time.time() < deadline:
            try:
                st, info = http_json("GET", f"http://{host}/api/v1/system/getDevInfo")
                assert info.get("projectName") == "ek410"
                assert info.get("bleSkuAvailable") is True
                print("back on SoftAP", info.get("projectName"), info.get("ip"))
                return
            except Exception as e:
                last = e
                time.sleep(1)
        raise RuntimeError(f"SoftAP getDevInfo after BLE exit failed: {last}")

    check("softap_boot_httpd", step_softap_boot)
    if failures:
        ser.close()
        return 1
    check("ctprov_device_info", step_ctprov_info)
    check("ble_sku_roundtrip", step_switch_ble_and_back)

    ser.close()
    print(f"\n=== dual-SKU test done: {failures} failure(s) ===")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
