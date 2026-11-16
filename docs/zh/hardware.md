# EK410 硬件说明

英文版：[hardware.md](../hardware.md)

| 项 | 值 |
|----|-----|
| 产品 | **EK410** · CamThink 7.09″ 电子墨水屏 |
| 主控 | ESP32-C61HR2 · 16MB Flash · 2MB PSRAM · CH340 |
| 面板 | Spectra 6 六色 · 逻辑分辨率 1600×1200 · 双 IC |
| 帧格式 | 960000 字节 · 4bpp · 每字节 2 像素 |
| 色码 | 黑 `0` 白 `1` 黄 `2` 红 `3` 蓝 `5` 绿 `6` |

## 引脚（`main/pindefine.h`）

| 功能 | GPIO |
|------|------|
| SPI CS0 / CS1 | 23 / 24 |
| SPI CLK / MOSI / MISO | 25 / 26 / 27 |
| EPD BUSY / RST | 29 / 12 |
| SPI 切换（墨水屏↔TF） | 8 |
| I2C SDA / SCL（AW9523B） | 22 / 28 |
| MAIN / PAGE | 2 / 3 |
| 电池 ADC | 4 |

## 按键

- **MAIN** 短按 / 长按开 SoftAP（约 2.5s）
- **PAGE** 翻页拨杆
- **芯片 BOOT** GPIO9 — 仅strapping，不是 SoftAP 键
- 硬件 **RESET** — EN/RST

## 电源 / 指示灯

- 电量：GPIO4 ADC，AW9523B 使能采样
- 状态 RGB：AW9523B；红闪多表示故障 / OTA 失败
