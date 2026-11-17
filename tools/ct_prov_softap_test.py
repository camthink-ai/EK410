#!/usr/bin/env python3
"""Automated CT-Prov/1 SoftAP binding tests for EK410 (NeoLink-compatible).

Targets POST /api/v1/ctprov on the device STA IP (or SoftAP 192.168.4.1).
Does not mutate Wi-Fi credentials by default (read-only ops + rejected writes).

Usage:
  python3 tools/ct_prov_softap_test.py [--host 192.168.31.42]
"""
from __future__ import annotations

import argparse
import json
import sys
import urllib.error
import urllib.request


def post_ctprov(host: str, envelope: dict, timeout: float = 15.0) -> tuple[int, dict | None, bytes]:
    url = f"http://{host}/api/v1/ctprov"
    body = json.dumps(envelope, separators=(",", ":")).encode("utf-8")
    req = urllib.request.Request(
        url,
        data=body,
        headers={"Content-Type": "application/json; charset=utf-8"},
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            raw = resp.read()
            status = resp.status
    except urllib.error.HTTPError as e:
        raw = e.read() if e.fp else b""
        return e.code, None, raw
    except Exception as e:
        print(f"FAIL transport: {e}")
        return -1, None, b""
    try:
        return status, json.loads(raw.decode("utf-8")), raw
    except Exception:
        return status, None, raw


def expect(cond: bool, msg: str) -> None:
    if not cond:
        raise AssertionError(msg)


def run(host: str) -> int:
    failures = 0

    def check(name: str, fn) -> None:
        nonlocal failures
        try:
            fn()
            print(f"PASS {name}")
        except Exception as e:
            failures += 1
            print(f"FAIL {name}: {e}")

    def t_device_info() -> None:
        st, j, _ = post_ctprov(host, {"id": 1, "op": "device.info.get", "data": {}})
        expect(st == 200 and j is not None, f"status={st} body={j}")
        expect(j["ok"] is True, "ok")
        expect(j["op"] == "device.info.get.result", j["op"])
        expect(j["data"]["model"] == "EK410", j["data"])
        expect(j["data"]["protocol_version"] == 1, "protocol_version")
        expect(isinstance(j["data"]["firmware"], str) and j["data"]["firmware"].startswith("v"), "fw")

    def t_capabilities() -> None:
        st, j, _ = post_ctprov(host, {"id": 2, "op": "communication.capabilities.get", "data": {}})
        expect(st == 200 and j and j["ok"], j)
        names = [s["name"] for s in j["data"]["sections"]]
        expect("wifi" in names and "mqtt" in names, names)
        wifi = next(s for s in j["data"]["sections"] if s["name"] == "wifi")
        expect(wifi["scannable"] is True, "wifi scannable")
        expect(set(wifi["fields"]) >= {"ssid", "password"}, wifi["fields"])

    def t_scan_page() -> None:
        st, j, _ = post_ctprov(
            host,
            {"id": 3, "op": "communication.scan", "data": {"section": "wifi", "cursor": ""}},
            timeout=30.0,
        )
        expect(st == 200 and j and j["ok"], j)
        expect(j["data"]["section"] == "wifi", j["data"])
        expect(isinstance(j["data"]["items"], list), "items")
        expect(isinstance(j["data"]["more"], bool), "more")
        expect(isinstance(j["data"]["cursor"], str), "cursor")

    def t_config_get() -> None:
        st, j, _ = post_ctprov(host, {"id": 4, "op": "config.get", "data": {}})
        expect(st == 200 and j and j["ok"], j)
        expect(j["op"] == "config.get.result", j["op"])

    def t_unsupported_section() -> None:
        st, j, _ = post_ctprov(
            host,
            {"id": 5, "op": "config.set", "data": {"cellular": {"apn": "x"}}},
        )
        expect(st == 200 and j and j["ok"] is False, j)
        expect(j["error"]["code"] == "SECTION_NOT_SUPPORTED", j)

    def t_field_not_supported() -> None:
        st, j, _ = post_ctprov(
            host,
            {
                "id": 6,
                "op": "config.set",
                "data": {"wifi": {"ssid": "x", "password": "password1", "bssid": "aa:bb:cc:dd:ee:ff"}},
            },
        )
        expect(st == 200 and j and j["ok"] is False, j)
        expect(j["error"]["code"] == "FIELD_NOT_SUPPORTED", j)

    def t_bad_envelope() -> None:
        st, j, raw = post_ctprov(host, {"id": 7, "op": "device.info.get"})  # missing data
        expect(st == 400, f"expected 400 got {st} {raw!r}")

    def t_unknown_op() -> None:
        st, j, _ = post_ctprov(host, {"id": 8, "op": "foo.bar", "data": {}})
        expect(st == 200 and j and j["ok"] is False, j)
        expect(j["error"]["code"] == "UNSUPPORTED_OPERATION", j)

    def t_bom_rejected() -> None:
        url = f"http://{host}/api/v1/ctprov"
        body = b"\xef\xbb\xbf" + b'{"id":9,"op":"device.info.get","data":{}}'
        req = urllib.request.Request(
            url, data=body, headers={"Content-Type": "application/json"}, method="POST"
        )
        try:
            with urllib.request.urlopen(req, timeout=10) as resp:
                st = resp.status
        except urllib.error.HTTPError as e:
            st = e.code
        expect(st == 400, f"BOM should 400, got {st}")

    for name, fn in [
        ("device.info.get", t_device_info),
        ("communication.capabilities.get", t_capabilities),
        ("communication.scan", t_scan_page),
        ("config.get", t_config_get),
        ("config.set SECTION_NOT_SUPPORTED", t_unsupported_section),
        ("config.set FIELD_NOT_SUPPORTED", t_field_not_supported),
        ("bad envelope → 400", t_bad_envelope),
        ("unknown op", t_unknown_op),
        ("BOM rejected", t_bom_rejected),
    ]:
        check(name, fn)

    print(f"\n{failures} failure(s)" if failures else "\nAll CT-Prov SoftAP tests passed.")
    return 1 if failures else 0


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--host", default="192.168.31.42")
    args = p.parse_args()
    return run(args.host)


if __name__ == "__main__":
    sys.exit(main())
