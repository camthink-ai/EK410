#!/usr/bin/env python3
"""EK410 single-firmware QA gate — unified entry for smoke / candidate / soak.

Reuses release_smoke.py, ct_prov_xor_test.py, stability_stress.py.
Writes machine-readable JSON (and optional JSONL). Power cases are
invalid/skipped until EK410_POWER_BACKEND is configured.

Exit codes:
  0  all cases pass (skipped/invalid OK)
  1  at least one fail_device or fail_fixture
  2  gate setup error (missing firmware, bad args)

Examples:
  python3 tools/qa_gate.py --tier smoke --port /dev/cu.wchusbserial3130 --host 192.168.31.42
  python3 tools/qa_gate.py --tier candidate --port PORT --host IP --json-out /tmp/qa.json
  python3 tools/qa_gate.py --tier soak --hours 2 --port PORT --host IP
"""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOOLS = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))

from qa_lib.firmware import firmware_meta  # noqa: E402
from qa_lib.power import open_power_meter_from_env  # noqa: E402
from qa_lib.result import CaseResult, Status, SuiteReport  # noqa: E402


def _run_script(
    script: str,
    args: list[str],
    timeout: float | None,
) -> tuple[int, str]:
    cmd = [sys.executable, str(TOOLS / script), *args]
    try:
        p = subprocess.run(
            cmd,
            cwd=str(ROOT),
            capture_output=True,
            text=True,
            timeout=timeout,
        )
        out = (p.stdout or "") + (p.stderr or "")
        return p.returncode, out
    except subprocess.TimeoutExpired as e:
        out = (e.stdout or "") + (e.stderr or "")
        if isinstance(out, bytes):
            out = out.decode("utf-8", "replace")
        return 124, out + "\n[qa_gate] timeout"
    except FileNotFoundError as e:
        return 127, str(e)


def _classify_script(rc: int, out: str, name: str, t0: float) -> CaseResult:
    dur = time.time() - t0
    tail = out[-4000:] if out else ""
    if rc == 0:
        return CaseResult(name, Status.PASS, "exit 0", dur, {"tail": tail[-500:]})
    if rc in (127, 126):
        return CaseResult(name, Status.FAIL_FIXTURE, f"runner error rc={rc}", dur, {"tail": tail})
    if rc == 124:
        return CaseResult(name, Status.FAIL_FIXTURE, "timeout", dur, {"tail": tail})
    # Heuristic: serial/port permission → fixture; assertion/FAIL → device
    low = out.lower()
    if any(
        x in low
        for x in (
            "could not open",
            "operation not permitted",
            "no such file",
            "pyserial required",
            "port is busy",
        )
    ):
        return CaseResult(name, Status.FAIL_FIXTURE, f"rc={rc}", dur, {"tail": tail})
    return CaseResult(name, Status.FAIL_DEVICE, f"rc={rc}", dur, {"tail": tail})


def _case_firmware_sha(report: SuiteReport, dist: Path, build: Path | None) -> None:
    t0 = time.time()
    meta = firmware_meta(dist)
    if not meta.get("present"):
        report.add(
            CaseResult(
                "firmware_artifact",
                Status.FAIL_FIXTURE,
                f"missing {dist}",
                time.time() - t0,
            )
        )
        return
    report.firmware_sha256 = meta["sha256"]
    report.firmware_path = meta["path"]
    detail = f"sha256={meta['sha256_12']}… bytes={meta['bytes']}"
    if build and build.is_file():
        bmeta = firmware_meta(build)
        if bmeta["sha256"] != meta["sha256"]:
            report.add(
                CaseResult(
                    "firmware_artifact",
                    Status.FAIL_FIXTURE,
                    f"dist≠build ({meta['sha256_12']} vs {bmeta['sha256_12']})",
                    time.time() - t0,
                    meta,
                )
            )
            return
        detail += " dist≡build"
    report.add(CaseResult("firmware_artifact", Status.PASS, detail, time.time() - t0, meta))


def _case_devinfo(report: SuiteReport, host: str) -> None:
    t0 = time.time()
    if not host:
        report.add(
            CaseResult("getDevInfo", Status.SKIPPED, "no --host", time.time() - t0)
        )
        return
    url = f"http://{host}/api/v1/system/getDevInfo"
    try:
        with urllib.request.urlopen(url, timeout=8) as r:
            info = json.loads(r.read().decode("utf-8"))
        report.device_id = str(info.get("deviceId") or "")
        if info.get("projectName") != "ek410":
            report.add(
                CaseResult(
                    "getDevInfo",
                    Status.FAIL_DEVICE,
                    f"projectName={info.get('projectName')!r}",
                    time.time() - t0,
                    info,
                )
            )
            return
        report.add(
            CaseResult(
                "getDevInfo",
                Status.PASS,
                f"ip={info.get('ip')} mqtt={info.get('mqttConnected')}",
                time.time() - t0,
                {
                    k: info.get(k)
                    for k in (
                        "projectName",
                        "fwVersion",
                        "ip",
                        "wifiConnected",
                        "mqttConnected",
                        "deviceId",
                    )
                },
            )
        )
    except Exception as e:
        report.add(
            CaseResult("getDevInfo", Status.FAIL_DEVICE, str(e), time.time() - t0)
        )


def _case_power_probe(report: SuiteReport) -> None:
    t0 = time.time()
    meter = open_power_meter_from_env()
    if not meter.available():
        report.add(
            CaseResult(
                "power_probe",
                Status.INVALID,
                f"no meter ({meter.backend_name()}); set EK410_POWER_BACKEND",
                time.time() - t0,
            )
        )
        return
    sample = meter.read("gate_probe")
    if sample is None or sample.current_ua is None:
        report.add(
            CaseResult(
                "power_probe",
                Status.INVALID,
                f"meter {meter.backend_name()} returned no current_ua",
                time.time() - t0,
            )
        )
        return
    report.add(
        CaseResult(
            "power_probe",
            Status.PASS,
            f"I={sample.current_ua} uA V={sample.voltage_v}",
            time.time() - t0,
            {
                "backend": meter.backend_name(),
                "current_ua": sample.current_ua,
                "voltage_v": sample.voltage_v,
                "panel_busy": sample.panel_busy,
                "rail_enable": sample.rail_enable,
            },
        )
    )


def _case_panel_cut_guard(report: SuiteReport) -> None:
    """Placeholder: require BUSY=1 ⇒ rail_enable must stay on when meter present."""
    t0 = time.time()
    meter = open_power_meter_from_env()
    if not meter.available():
        report.add(
            CaseResult(
                "panel_rail_during_busy",
                Status.INVALID,
                "needs power/BUSY fixture",
                time.time() - t0,
            )
        )
        return
    sample = meter.read("busy_guard")
    if sample is None or sample.panel_busy is None or sample.rail_enable is None:
        report.add(
            CaseResult(
                "panel_rail_during_busy",
                Status.INVALID,
                "sample missing panel_busy or rail_enable",
                time.time() - t0,
            )
        )
        return
    if sample.panel_busy and not sample.rail_enable:
        report.add(
            CaseResult(
                "panel_rail_during_busy",
                Status.FAIL_DEVICE,
                "BUSY asserted but rail_enable off (panel cut during refresh)",
                time.time() - t0,
                sample.raw or {},
            )
        )
        return
    report.add(
        CaseResult(
            "panel_rail_during_busy",
            Status.PASS,
            f"busy={sample.panel_busy} rail={sample.rail_enable}",
            time.time() - t0,
        )
    )


def _run_release_smoke(report: SuiteReport, port: str, host: str, idle_sec: int, skip_reset: bool) -> None:
    t0 = time.time()
    args = ["--port", port, "--idle-sec", str(idle_sec)]
    if host:
        args += ["--host", host]
    if skip_reset:
        args.append("--skip-reset")
    rc, out = _run_script("release_smoke.py", args, timeout=900)
    report.add(_classify_script(rc, out, "release_smoke", t0))
    print(out[-8000:] if out else "", end="" if out.endswith("\n") else "\n")


def _run_xor(report: SuiteReport, port: str, host: str, rounds: int, idle_sec: int) -> None:
    for i in range(max(1, rounds)):
        t0 = time.time()
        args = ["--port", port, "--idle-sec", str(idle_sec)]
        if host:
            args += ["--host", host]
        rc, out = _run_script("ct_prov_xor_test.py", args, timeout=300)
        name = f"ct_prov_xor[{i + 1}/{rounds}]"
        report.add(_classify_script(rc, out, name, t0))
        print(f"--- {name} rc={rc} ---")
        if out:
            print(out[-3000:], end="" if out.endswith("\n") else "\n")
        if rc != 0:
            break


def _run_stress(report: SuiteReport, port: str, host: str, rounds: int) -> None:
    t0 = time.time()
    args = ["--rounds", str(rounds)]
    if host:
        args += ["--host", host]
    if port:
        args += ["--port", port]
    # stress can run long with uploads
    rc, out = _run_script("stability_stress.py", args, timeout=3600)
    report.add(_classify_script(rc, out, "stability_stress", t0))
    print(out[-6000:] if out else "", end="" if (out or "").endswith("\n") else "\n")


def _run_soak(report: SuiteReport, port: str, host: str, hours: float, stress_rounds: int) -> None:
    deadline = time.time() + max(0.05, hours) * 3600.0
    n = 0
    while time.time() < deadline:
        n += 1
        left = deadline - time.time()
        print(f"\n======== soak loop {n} ({left / 3600:.2f}h left) ========", flush=True)
        t0 = time.time()
        args = ["--rounds", str(max(2, stress_rounds))]
        if host:
            args += ["--host", host]
        if port:
            args += ["--port", port]
        # Cap each loop so we can re-enter before deadline
        rc, out = _run_script(
            "stability_stress.py",
            args,
            timeout=min(3600.0, max(120.0, left)),
        )
        report.add(_classify_script(rc, out, f"soak_stress[{n}]", t0))
        if out:
            print(out[-2000:], end="" if out.endswith("\n") else "\n")
        # Brief XOR between stress loops when serial available
        if port and time.time() < deadline:
            t1 = time.time()
            xargs = ["--port", port, "--idle-sec", "12"]
            if host:
                xargs += ["--host", host]
            xrc, xout = _run_script("ct_prov_xor_test.py", xargs, timeout=240)
            report.add(_classify_script(xrc, xout, f"soak_xor[{n}]", t1))
        if rc != 0:
            # Continue soak unless fixture broke hard
            last = report.cases[-1]
            if last.status == Status.FAIL_FIXTURE and "could not open" in (last.detail + str(last.meta)).lower():
                break
        time.sleep(5)


def main() -> int:
    ap = argparse.ArgumentParser(description="EK410 QA gate (smoke/candidate/soak)")
    ap.add_argument(
        "--tier",
        choices=("smoke", "candidate", "soak"),
        default="smoke",
        help="smoke=release_smoke; candidate=+xor rounds+stress; soak=hours loop",
    )
    ap.add_argument("--port", default="", help="USB serial (required for smoke/xor)")
    ap.add_argument("--host", default="", help="STA IP, e.g. 192.168.31.42")
    ap.add_argument("--idle-sec", type=int, default=18)
    ap.add_argument("--skip-reset", action="store_true")
    ap.add_argument("--xor-rounds", type=int, default=3, help="candidate XOR repeats")
    ap.add_argument("--stress-rounds", type=int, default=4)
    ap.add_argument("--hours", type=float, default=72.0, help="soak duration")
    ap.add_argument(
        "--dist",
        type=Path,
        default=ROOT / "dist" / "ek410.bin",
        help="release image to fingerprint",
    )
    ap.add_argument(
        "--build",
        type=Path,
        default=ROOT / "build" / "ek410.bin",
        help="optional build tree image for SHA match",
    )
    ap.add_argument("--json-out", type=Path, default=None)
    ap.add_argument("--jsonl-out", type=Path, default=None)
    ap.add_argument(
        "--skip-power",
        action="store_true",
        help="do not even probe power stub (record skipped)",
    )
    args = ap.parse_args()

    if args.tier in ("smoke", "candidate") and not args.port:
        print("FAIL: --port required for smoke/candidate", file=sys.stderr)
        return 2
    if args.tier == "soak" and not args.port and not args.host:
        print("FAIL: soak needs --port and/or --host", file=sys.stderr)
        return 2

    report = SuiteReport(
        tier=args.tier,
        host=args.host,
        port=args.port,
    )

    print(f"==> qa_gate tier={args.tier}", flush=True)
    _case_firmware_sha(report, args.dist, args.build if args.build else None)
    if any(c.name == "firmware_artifact" and c.status != Status.PASS for c in report.cases):
        _finish(report, args)
        return 2 if not report.ok else 0

    if args.host:
        _case_devinfo(report, args.host)

    if args.skip_power:
        report.add(CaseResult("power_probe", Status.SKIPPED, "--skip-power"))
        report.add(CaseResult("panel_rail_during_busy", Status.SKIPPED, "--skip-power"))
    else:
        _case_power_probe(report)
        _case_panel_cut_guard(report)

    if args.tier == "smoke":
        if not args.port:
            report.add(CaseResult("release_smoke", Status.FAIL_FIXTURE, "missing --port"))
        else:
            _run_release_smoke(report, args.port, args.host, args.idle_sec, args.skip_reset)
    elif args.tier == "candidate":
        if args.port:
            _run_release_smoke(report, args.port, args.host, args.idle_sec, args.skip_reset)
            _run_xor(report, args.port, args.host, args.xor_rounds, args.idle_sec)
        else:
            report.add(CaseResult("release_smoke", Status.FAIL_FIXTURE, "missing --port"))
        if args.host:
            _run_stress(report, args.port, args.host, args.stress_rounds)
        else:
            report.add(CaseResult("stability_stress", Status.SKIPPED, "no --host"))
    elif args.tier == "soak":
        if not args.host and not args.port:
            report.add(CaseResult("soak", Status.FAIL_FIXTURE, "need --host and/or --port"))
        else:
            _run_soak(report, args.port, args.host, args.hours, args.stress_rounds)

    return _finish(report, args)


def _finish(report: SuiteReport, args: argparse.Namespace) -> int:
    report.finished_at = time.time()
    summary = report.to_dict()
    print("\n======== QA GATE SUMMARY ========", flush=True)
    print(
        f"tier={report.tier} ok={report.ok} sha={report.firmware_sha256[:12] or '-'}… "
        f"counts={report.counts()}",
        flush=True,
    )
    for c in report.cases:
        print(f"  [{c.status.value:12}] {c.name}: {c.detail}", flush=True)
    if args.json_out:
        report.write_json(args.json_out)
        print(f"wrote {args.json_out}", flush=True)
    if args.jsonl_out:
        report.write_jsonl(args.jsonl_out)
        print(f"wrote {args.jsonl_out}", flush=True)
    if not report.ok:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
