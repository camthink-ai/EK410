# SoftAP / LAN HTTP API

Base URL: SoftAP `http://192.168.4.1` or device STA IP after joining home Wi‑Fi.

Most JSON handlers return a flat object. Errors: `{"code":1,"msg":"..."}`.

Companion: [API（中文）](zh/api.md)

## System

| Method | Path | Notes |
|--------|------|--------|
| GET | `/api/v1/system/getDevInfo` | name, MAC, deviceId, fwVersion, Wi‑Fi/MQTT/SoftAP flags, battery |
| POST | `/api/v1/system/setDevInfo` | `{devName}` |
| POST | `/api/v1/system/setDevReset` | reboot |
| POST | `/api/v1/system/setDevFactory` | factory wipe (keeps deviceId) |
| POST | `/api/v1/system/setDevUpgrade` | `{url}` HTTPS OTA |
| GET/POST | `/api/v1/system/getPowerParam` / `setPowerParam` | `mode`, `sleepMin`, `wakeSec` (30–600, default 120), `wakeUrl`, `imageQuality` |
| POST | `/api/v1/system/sleepNow` | enter deep sleep |

## Network

| Method | Path | Notes |
|--------|------|--------|
| GET/POST | `/api/v1/network/getWifiParam` / `setWifiParam` | `{ssid,password}` |
| GET | `/api/v1/network/scanWifi` | AP list |
| GET/POST | `/api/v1/network/getMqttParam` / `setMqttParam` | host, port, transport (`mqtt`\|`mqtts`), credentials; MQTTS: `tlsVerify` (default true), optional `caPem` (custom CA; empty = system CRT bundle). SoftAP replies may include `mqttSoftApConnect` / `mqttSoftApSession` / `mqttDeferred` — SPA polls `getDevInfo`, does not call `testMqtt` |
| POST | `/api/v1/network/testMqtt` | probe (`connected` requires STA). SoftAP: async / already-up only — prefer polling `getDevInfo` |
| POST | `/api/v1/network/openSoftAp` / `closeSoftAp` | SoftAP control |
| POST | `/api/v1/ctprov` | [NeoLinker](https://github.com/camthink-ai/NeoLinker) **CT-Prov/1** SoftAP binding (whole JSON envelope; see `docs/ct_prov.md`) |

## Display

| Method | Path | Notes |
|--------|------|--------|
| POST | `/api/v1/display/renderText` | simple text |
| POST | `/api/v1/display/renderCanvas` | `{bg,items[]}` text/rect/qr |
| POST | `/api/v1/display/uploadFrame` | raw **960000**-byte panel frame |
| POST | `/api/v1/display/displayUrl` | device fetch + decode + refresh (MQTT / wake) |
| POST | `/api/v1/display/proxyImage` | `{url}` → image bytes for **Web preview** |
| GET | `/api/v1/display/getLastFrameMeta` | `{present,seq,source,savedUs,bytes,crc32}` |
| GET | `/api/v1/display/lastFrame` | raw **960000**-byte native 4bpp (same as `uploadFrame`) |
| POST | `/api/v1/display/testPattern` | test pattern |

### Last frame (panel snapshot)

After a successful **user** refresh (`uploadFrame`, `displayUrl` / wake URL, canvas, text, …), the device stores the native panel frame in flash partition `lastfrm`. SoftAP setup / splash / boot screens are not saved. SoftAP **Overview** shows the snapshot; Display “Load from panel” pulls it into the editor. Clients can also `GET lastFrame` anytime.

### Web image path (recommended on SoftAP)

1. `proxyImage` / local file / `lastFrame` → preview in browser  
2. Client dither → `uploadFrame`

### MQTT / wake path

- Command `display_url` / power `wakeUrl` → `displayUrl` on device.

## NeoMind MQTT

- Device type: `ek410_Eink`
- Client ID: `ek410-{deviceId}`
- Topics: `device/ek410_Eink/{deviceId}/uplink` ↔ `.../downlink`
