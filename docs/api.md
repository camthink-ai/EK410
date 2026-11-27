# SoftAP / LAN HTTP API

Source: [github.com/camthink-ai/EK410](https://github.com/camthink-ai/EK410)

Base URL: SoftAP `http://192.168.4.1`, or the device's STA IP after joining home Wi‑Fi.

Most JSON handlers return a flat object. Errors: `{"code":1,"msg":"..."}`.

**There is no authentication**: every route below is open to anyone who can reach the device on either interface — see [security.md](security.md).

Limits: JSON bodies ≤ 16 KB and ≤ 8 nesting levels (deeper input is rejected before parsing); CT-Prov bodies ≤ 4 KB.

Companion: [API（中文）](zh/api.md)

Product-facing API notes and examples: [CamThink Wiki · HTTP API](https://wiki.camthink.ai/docs/epaper-ek410-series/software-guide/ek410-http-api) · [中文](https://wiki.camthink.ai/zh-Hans/docs/epaper-ek410-series/software-guide/ek410-http-api)

## System

| Method | Path | Notes |
|--------|------|-------|
| GET | `/api/v1/system/getDevInfo` | `devName`, `devMac`, `deviceId`, `fwVersion`, `ip`, `wifiConnected`, `wifiState`, `mqttConnected`, `mqttStarted`, `mqttReason`, `mqttLastError`, `httpServerUp`, `softapUp`, `bleProvUp`, `softApSsid` (plus `softApPass` while the SoftAP is up), `provisioned`, `panelOk`, `displayBusy`, `powerMode`, `sleepMin`, `wakeSec`, `uptime_s`, `batterySense` / `batteryMv` / `batteryPercent`, `otaPartition`, `otaPendingVerify`, **`otaState`** (`idle` \| `running` \| `failed`) and `otaError` after a failed attempt |
| GET | `/api/v1/system/getDiag` | On-demand diagnostics for validating a build on hardware: `heap` (`free`, `minFree`, `freeInt`, `minFreeInt`, `largestInt`, `freeSpiram`, `largestSpiram` — bytes) and `stackMin` (smallest free stack ever seen, bytes, per long-lived task) |
| POST | `/api/v1/system/setDevInfo` | `{devName}` — 1..32 bytes of UTF‑8; longer names are rejected (`devName too long (max 32 bytes)`) |
| POST | `/api/v1/system/setDevReset` | reboot |
| POST | `/api/v1/system/setDevFactory` | wipe config, BLE bonds and the event log (keeps `deviceId`), then reboot |
| POST | `/api/v1/system/setDevUpgrade` | `{url}` (≤ 511 chars) — HTTPS-only OTA. Replies `{code:0}` **before** the download: poll `getDevInfo.otaState`; `failed` carries `otaError`, otherwise the device reboots into the new image. A failed attempt restores MQTT and the sleep schedule by itself |
| POST | `/api/v1/system/uploadFirmware` | raw `application/octet-stream` app image, 200 KB – 4 MB, `Content-Length` required; refused with `ota busy` while another OTA is running |
| GET | `/api/v1/system/getEventLogMeta` | `{present,bytes,seq,capBytes,keepDays,timeOk}` |
| GET | `/api/v1/system/eventLog` | text/plain download |
| POST | `/api/v1/system/clearEventLog` | wipe the log (`event log clear failed` on error) |
| GET | `/api/v1/system/getPowerParam` | `mode`, `sleepMin`, `wakeSec`, `wakeUrl`, `imageQuality` |
| POST | `/api/v1/system/setPowerParam` | same fields; **only fields present in the body are changed**. `mode` `always` \| `cycle` \| `hibernate`, `sleepMin` 1..525600 (cycle), `wakeSec` 30–600 (default 120), `imageQuality` `quality` \| `speed` — how a URL picture is dithered: one ink dot per panel pixel (about 6 s) \| 800×600, 2×2 dots. Replies `sleepArmed`, plus `sleepDeferred: "softap"` or `"no_sta"` when hibernate / the cycle cannot start yet (it starts when the SoftAP closes / STA gets an IP). A body with neither `mode` nor `sleepMin` leaves the sleep schedule untouched |
| POST | `/api/v1/system/sleepNow` | deep sleep now; refused while the SoftAP is up |

## Network

| Method | Path | Notes |
|--------|------|-------|
| GET | `/api/v1/network/getWifiParam` | `ssid`, `hasPassword`, `isConnected`, `state`, `ip` — the password is never returned |
| POST | `/api/v1/network/setWifiParam` | `{ssid,password}`; a blank password keeps the stored one for the same SSID |
| GET | `/api/v1/network/scanWifi` | `{aps:[{ssid,rssi,secure,authmode}]}` |
| GET | `/api/v1/network/getMqttParam` | `host`, `port`, `transport` (`mqtt` \| `mqtts`), `tls`, `tlsVerify` (default true), `caPem`, `clientId`, `qos`, `username`, `hasPassword`, `topicUp`, `topicDown`, `connected` — the password is never returned |
| POST | `/api/v1/network/setMqttParam` | same fields. A blank password keeps the stored one **only while host and username are unchanged**; changing either clears it and the reply carries `warn`. MQTTS: `tlsVerify`, optional `caPem` (custom CA; empty = system CRT bundle). SoftAP replies may add `mqttSoftApConnect` / `mqttSoftApSession` / `mqttDeferred` — the SPA polls `getDevInfo` instead of calling `testMqtt` |
| POST | `/api/v1/network/testMqtt` | connect probe (`connected` requires STA). SoftAP: async / already-up only |
| POST | `/api/v1/network/openSoftAp` / `closeSoftAp` | SoftAP control (reply before the switch completes; poll `getDevInfo.softapUp`) |
| POST | `/api/v1/network/openBleProv` / `closeBleProv` | BLE CT-Prov control; `openBleProv` takes an optional `{idleSec}` override for CI |
| POST | `/api/v1/ctprov` | [NeoLinker](https://github.com/camthink-ai/NeoLinker) **CT-Prov/1** binding (JSON envelope `{id,op,data}`; ops `device.info.get`, `communication.capabilities.get`, `communication.scan`, `config.set`, `config.get`, `session.close`; see [ct_prov.md](ct_prov.md)). Reachable on the STA IP as well |

## Display

| Method | Path | Notes |
|--------|------|-------|
| POST | `/api/v1/display/renderText` | `{text,color,bg,scale}` |
| POST | `/api/v1/display/renderCanvas` | `{bg,items[]}` text / rect / image / qr (≤ 48 items, ≤ 2 QR) |
| POST | `/api/v1/display/uploadFrame` | raw **960000**-byte native frame (layout below). Refused with `panel busy` while a display is running or queued — it does not wait for the panel; push the frame again once idle |
| POST | `/api/v1/display/displayUrl` | `{url}` device fetch + decode + refresh (JPEG / PNG / raw; http and https). Follows up to 5 redirects (an https URL only redirects to https). A JPEG or PNG over 700 KB is refused before the body is read (`image >700KB limit`), and the larger the file the smaller the decode: a 1600×1200 JPEG is decoded to 400×300 up to about 400 KB and to 200×150 above (the body is still in PSRAM next to the RGB buffer). It is then upsampled smoothly and dithered at panel resolution — one ink dot per panel pixel with `imageQuality: quality`, 800×600 (2×2 dots) with `speed`; grey content only takes black and white. Send a raw frame for anything bigger, or for full quality. Other bodies are buffered in PSRAM: nominal cap 1280 KB. If one free block is not enough (e.g. a web-poll TCP control block split the heap), the file is read into up to three pieces and JPEG/PNG decode from those pieces in order; a raw 960000-byte frame uses the same path. A file that still cannot fit is rejected before the body is read and the previous frame stays (`file >1280KB limit`, `need …KB, got …KB in pieces`). A response without a `Content-Length` (chunked, HTTP/1.0) is accepted up to the 1280 KB cap and decoded like any other (until 1.3.10 it was decoded to 67×50). Only baseline JPEG with three components (colour) is decoded; a progressive JPEG is refused with `progressive JPEG not supported` and a grayscale (single-component) one with `grayscale JPEG not supported` (until 1.3.10 both said `JPEG decode fail`). The EXIF orientation tag of a JPEG is applied (1.3.11): a portrait photo as a phone stores it comes out upright, whatever the size of the picture. A picture that is not 1600×1200 is scaled to cover the panel and cropped; transparent pixels of a PNG come out white |
| POST | `/api/v1/display/proxyImage` | `{url}` → image bytes for the **web preview** |
| GET | `/api/v1/display/getLastFrameMeta` | `{present,seq,source,savedUs,bytes,crc32}` |
| GET | `/api/v1/display/lastFrame` | raw **960000**-byte native 4bpp frame (same layout as `uploadFrame`) |
| POST | `/api/v1/display/testPattern` | test pattern |

### Native frame layout

Two driver ICs side by side: the frame is the **CS0 half** (native x 0..599)
followed by the **CS1 half** (x 600..1199), each 1600 lines × 300 bytes; every
byte holds two native pixels, high nibble = left one. Logical landscape
1600×1200 is that raster rotated 90° CCW (`lx = ny`, `ly = 1199 − nx`). Colour
codes: black `0x0`, white `0x1`, yellow `0x2`, red `0x3`, blue `0x5`, green
`0x6`. [`tools/frame_convert.py`](../tools/frame_convert.py) produces it.

### Last frame (panel snapshot)

After a successful **user** refresh (`uploadFrame`, `displayUrl` / wake URL, canvas, text, …), the device stores the native panel frame in flash partition `lastfrm` (skipped when the frame is unchanged). SoftAP setup / splash / boot screens are not saved. SoftAP **Overview** shows the snapshot; Display "Load from panel" pulls it into the editor.

### Web image path (recommended on SoftAP)

1. `proxyImage` / local file / `lastFrame` → preview in browser  
2. Client dither → `uploadFrame`

### MQTT / wake path

- Command `display_url` / power `wakeUrl` → `displayUrl` on device.

## NeoMind MQTT

- Device type: `ek410_Eink`
- Client ID: `ek410-{deviceId}`
- Topics: `device/ek410_Eink/{deviceId}/uplink` ↔ `.../downlink`
- Downlink (`{"cmd":"...","params":{...}}`): `ping` / `heartbeat`, `clear`, `render_text`, `render_canvas`, `display_url`, `reboot`, `sleep_now` / `sleep`, `set_power`. Display commands ack `queued`, then a completion `cmd_ack`; a sleep that cannot start gets a follow-up `cmd_ack` with `error`.
