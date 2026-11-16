# EK410 Hardware Notes

Companion: [硬件说明（中文）](zh/hardware.md)

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

- **MAIN** short / long (SoftAP ~2.5s) — see `button.c`
- **PAGE** rocker (same GPIO)
- **Chip BOOT** GPIO9 — strapping only, not SoftAP
- Hardware **RESET** — EN/RST, not a firmware GPIO

## Power / LED

- Battery sense: ADC GPIO4, enable via AW9523B
- Status RGB: AW9523B (`led.c`); red blink = fault / OTA fail paths
