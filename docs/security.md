# Security notes

Source: [github.com/camthink-ai/EK410](https://github.com/camthink-ai/EK410)

Status as of firmware 1.3.10. Read this before deploying units outside a lab
or before the first production flash.

## What is protected

- **HTTPS OTA** verifies the server certificate against the bundled CA list,
  accepts `https://` only, and refuses a redirect down to `http://`.
- **HTTPS image fetch** (`displayUrl`, `wakeUrl`, `proxyImage`) verifies the
  server certificate too. Plain `http://` image URLs are allowed; their
  content has no integrity protection, but decoding is bounds- and
  size-checked.
- **MQTTS** verifies by default (`tlsVerify`: bundled CAs, or a custom CA PEM).
  "Verify off" disables verification and is labelled insecure in the web UI.
- **BLE CT-Prov** requires an encrypted link (pairing) before its GATT
  characteristics can be read or written.
- **Secrets are write-only over the API**: `getWifiParam` and `getMqttParam`
  return `hasPassword`, never the password.
- **Input limits**: HTTP bodies ≤ 16 KB, CT-Prov bodies ≤ 4 KB, JSON nesting
  ≤ 8 levels, image dimensions and allocation sizes bounded.

## What is not protected (current design)

- **The HTTP API has no authentication.** The same 32 routes are served on the
  SoftAP (`192.168.4.1`) and on the LAN (the STA IP). Anyone who can reach the
  device can change Wi-Fi and MQTT settings (including pointing MQTT at
  another broker), flash arbitrary firmware (`uploadFirmware`,
  `setDevUpgrade`), factory reset, reboot, force deep sleep, push images, and
  read the event log and device info.
- **The SoftAP password is the fixed `12345678`** on every unit.
- **Firmware images are not signed** (no Secure Boot) and **flash / NVS are
  not encrypted**: credentials at rest are readable with physical access, and
  OTA trust rests on TLS server authentication plus on nobody untrusted
  reaching the upload route.
- **BLE pairing is Just-Works** (no MITM protection): any nearby phone can
  provision a device whose BLE window is open.
- **MQTT downlink commands run as-is.** Whoever can publish to the device's
  downlink topic controls it (`reboot`, `sleep_now`, `set_power`,
  `display_url`, …).

## Decisions to take before mass production

1. **API authentication.** Minimum: a per-device token (on the label or shown
   on the SoftAP page) required for the destructive routes — `uploadFirmware`,
   `setDevUpgrade`, `setDevFactory`, `setDevReset`, `setWifiParam`,
   `setMqttParam`, `setPowerParam`, `sleepNow`, `openSoftAp`, `openBleProv`,
   and CT-Prov `config.set`.
2. **A per-unit SoftAP password** (derived from the device id, shown on the
   setup screen) instead of `12345678`.
3. **Secure Boot V2 with signed app images, flash encryption and NVS
   encryption.** These burn eFuses, need key management and a factory flashing
   step, and cannot be added to units already in the field without physical
   access — they have to be decided before the first production flash.
4. **Lock the UART download mode** (eFuse) once 3 is in place.
5. Until 1–3 exist, deploy units on a segregated LAN / VLAN / SSID: the LAN
   interface exposes the same unauthenticated API as the hotspot.
