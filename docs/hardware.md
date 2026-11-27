# EK410 Hardware Notes

Companion: [硬件说明（中文）](zh/hardware.md) · Source: [github.com/camthink-ai/EK410](https://github.com/camthink-ai/EK410)

Full product hardware guide (specs, keys, LED, panel): [CamThink Wiki](https://wiki.camthink.ai/docs/epaper-ek410-series/hardware-guide/ek410-specifications) · [中文](https://wiki.camthink.ai/zh-Hans/docs/epaper-ek410-series/hardware-guide/ek410-specifications)

| Item | Value |
|------|--------|
| Product | **EK410** · CamThink 7.09″ E-Ink Display |
| MCU | ESP32-C61HR2 · 16MB flash · 2MB PSRAM · CH340 USB-UART |
| Panel | E Ink Spectra 6 · 1600×1200 logical · dual IC · 6 colors |
| Frame | 960000 bytes (CS0 + CS1 halves) · 4bpp · 2 px/byte |
| Colors | Black `0` White `1` Yellow `2` Red `3` Blue `5` Green `6` |

## Pins (`main/pindefine.h`)

| Function | GPIO |
|----------|------|
| SPI CS0 / CS1 | 23 / 24 |
| SPI CLK / MOSI / MISO | 25 / 26 / 27 |
| EPD BUSY / RST | 29 / 12 |
| SPI mux (EPD↔TF) | 8 |
| I2C SDA / SCL (AW9523B) | 22 / 28 |
| MAIN / PAGE buttons | 2 / 3 |
| Battery ADC | 4 |

## Buttons

- **MAIN** hold ~2.5 s → SoftAP. A short press leaves SoftAP or BLE provisioning when either is up (1.3.11); otherwise it has no action of its own (it only cancels / re-arms the CYCLE window) — see `button.c`
- **PAGE** rocker (same GPIO) — hold ~2.5 s → BLE CT-Prov on / off
- **MAIN + PAGE** hold ~2.5 s then release → power off (deep sleep, MAIN wake only). Hold past ~6 s → factory reset (overrides power-off; both keys for the continuous 6 s)
- **Chip BOOT** GPIO9 — boot-mode strap. Sampled once ~50 ms after boot: low → service factory reset. Held across a reset it enters the ROM download mode instead
- Hardware **RESET** — EN/RST, not a firmware GPIO
- Deep-sleep wake (MAIN / PAGE / timer) reconnects only — does **not** auto-open SoftAP or BLE

## SoftAP web console

- Display modes share a **横 / 竖** orientation toggle (default landscape 1600×1200; portrait 1200×1600). Preference is stored in the browser `localStorage` only
- Portrait push uses `uploadFrame` (firmware `renderCanvas` stays landscape)

## Power / LED

- Battery sense: ADC GPIO4, enable via AW9523B
- Status RGB: AW9523B (`led.c`); red pulse = fault / OTA fail paths (never sticky)
- Deep-sleep wake: orange blink during boot panel paint, then normal join / green policy
