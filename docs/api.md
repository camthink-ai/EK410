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
| GET/POST | `/api/v1/system/getPowerParam` / `setPowerParam` | `mode`, `sleepMin`, `wakeUrl`, `imageQuality` |
| POST | `/api/v1/system/sleepNow` | enter deep sleep |

## Network

| Method | Path | Notes |
|--------|------|--------|
| GET/POST | `/api/v1/network/getWifiParam` / `setWifiParam` | `{ssid,password}` |
| GET | `/api/v1/network/scanWifi` | AP list |
| GET/POST | `/api/v1/network/getMqttParam` / `setMqttParam` | host, port, transport, credentials |
| POST | `/api/v1/network/testMqtt` | probe |
| POST | `/api/v1/network/openSoftAp` / `closeSoftAp` | SoftAP control |

## Display

| Method | Path | Notes |
|--------|------|--------|
| POST | `/api/v1/display/renderText` | simple text |
| POST | `/api/v1/display/renderCanvas` | `{bg,items[]}` text/rect/qr |
| POST | `/api/v1/display/uploadFrame` | raw **960000**-byte panel frame |
| POST | `/api/v1/display/displayUrl` | device fetch + decode + refresh (MQTT / wake) |
| POST | `/api/v1/display/proxyImage` | `{url}` → image bytes for **Web preview** |
| POST | `/api/v1/display/testPattern` | test pattern |

### Web image path (recommended on SoftAP)

1. `proxyImage` (or local file) → preview in browser  
2. Client dither → `uploadFrame`

### MQTT / wake path

- Command `display_url` / power `wakeUrl` → `displayUrl` on device.

## NeoMind MQTT

- Device type: `ek410_Eink`
- Client ID: `ek410-{deviceId}`
- Topics: `device/ek410_Eink/{deviceId}/uplink` ↔ `.../downlink`
