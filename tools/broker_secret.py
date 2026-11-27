"""Take the broker credentials a unit already holds, to stand on the broker's
side of it during a test. The device API reports `hasPassword` only (since
1.3.10), and the broker refuses a client without the password.

Reads the NVS partition over serial — the unit is reset for that, nothing is
written, its configuration stays as it is — picks the MQTT entries of
namespace "epcfg", and stores them in a file for the tools that need them:
mode 0600, outside the repository. Only key names and lengths are printed;
the dump is deleted.

    python3 tools/broker_secret.py /dev/cu.wchusbserialXXXX ~/.ek410-broker.json

soak_long.py --broker-secret reads that file. Do not commit it, do not
paste it anywhere: it is the unit's password.
"""
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

PORT, SECRET = sys.argv[1], Path(sys.argv[2]).expanduser()
NVS_OFFSET, NVS_SIZE, NS = 0x9000, 0x6000, "epcfg"
PAGE, ENTRIES = 4096, 126          # a page: 32 B header, 32 B state bitmap, 126 × 32 B
WRITTEN = 2                        # entry state: 3 empty, 2 written, 1 erased, 0 illegal
T_U8, T_U32, T_I32, T_STR = 0x01, 0x04, 0x14, 0x21


def parse(dump: bytes) -> dict:
    """Newest value of every key of every namespace: {namespace: {key: value}}.
    Entry layout of ESP-IDF 5.5 (nvs_partition_tool/nvs_parser.py): namespace
    index, type, span, chunk index, CRC32, 16-byte key, 8-byte data."""
    names, written = {}, []
    for page in range(len(dump) // PAGE):
        base = page * PAGE
        if dump[base:base + 4] == b"\xff\xff\xff\xff":
            continue                            # never used
        seq = int.from_bytes(dump[base + 4:base + 8], "little")
        bitmap = dump[base + 32:base + 64]      # 2 bits per entry, low bits first
        i = 0
        while i < ENTRIES:
            if (bitmap[i // 4] >> ((i % 4) * 2)) & 0x03 != WRITTEN:
                i += 1
                continue
            off = base + 64 + i * 32
            entry = dump[off:off + 32]
            span = entry[2] or 1
            written.append((seq, i, entry[0], entry[1], bytes(entry[8:24]), off, span))
            i += span
    out: dict = {}
    for seq, i, ns, etype, key, off, span in sorted(written):
        text = key.split(b"\0", 1)[0].decode("utf-8", "replace")
        data = dump[off + 24:off + 32]
        if ns == 0 and etype == T_U8:           # the namespace itself: name → index
            names[data[0]] = text
            continue
        name = names.get(ns, f"ns{ns}")
        if etype == T_STR:
            if span == 1:
                value = data.split(b"\0", 1)[0].decode("utf-8", "replace")
            else:                               # length first, then the text entry by entry
                n = int.from_bytes(data[0:2], "little")
                value = dump[off + 32:off + 32 + (span - 1) * 32][:n].decode(
                    "utf-8", "replace").rstrip("\0")     # n counts the terminator
        elif etype == T_I32:
            value = int.from_bytes(data[0:4], "little", signed=True)
        elif etype == T_U32:
            value = int.from_bytes(data[0:4], "little")
        elif etype == T_U8:
            value = data[0]
        else:
            value = f"<type 0x{etype:02x}>"
        out.setdefault(name, {})[text] = value
    return out


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="ek410_nvs_") as tmp:
        dump = Path(tmp) / "nvs.bin"
        cmd = [sys.executable, "-m", "esptool", "--chip", "esp32c61", "-p", PORT, "-b", "115200",
               "--before", "default_reset", "--after", "hard_reset",
               "read_flash", hex(NVS_OFFSET), hex(NVS_SIZE), str(dump)]
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=180)
        if r.returncode != 0 or not dump.exists() or dump.stat().st_size != NVS_SIZE:
            sys.exit(f"NVS read failed (exit {r.returncode}): {r.stderr.strip()[-300:]}")
        data = dump.read_bytes()
    cfg = parse(data)
    print("namespaces:", ", ".join(f"{n} ({len(k)} keys)" for n, k in cfg.items()))
    if NS not in cfg:
        sys.exit(f"namespace {NS!r} not found in the NVS partition (have: {sorted(cfg)})")
    have = cfg[NS]
    print("epcfg keys: " + ", ".join(
        f"{k}:{'str' if isinstance(v, str) else type(v).__name__}"
        + (f"[{len(v)}]" if isinstance(v, str) else "") for k, v in sorted(have.items())))
    missing = [k for k in ("host", "port", "cid", "mquser", "mqpass") if k not in have]
    if missing:
        sys.exit(f"missing in {NS}: {missing}")
    SECRET.parent.mkdir(parents=True, exist_ok=True)
    fd = os.open(SECRET, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w", encoding="utf-8") as f:
        json.dump({"host": have["host"], "port": have["port"], "clientId": have["cid"],
                   "username": have["mquser"], "password": have["mqpass"]}, f)
    os.chmod(SECRET, 0o600)
    print(f"broker credentials stored in {SECRET} (mode 0600): host={have['host']} "
          f"port={have['port']} user={have['mquser']} password=<{len(have['mqpass'])} chars, hidden>")
