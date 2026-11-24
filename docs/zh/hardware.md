# EK410 硬件说明

英文版：[hardware.md](../hardware.md) · 源码：[github.com/camthink-ai/EK410](https://github.com/camthink-ai/EK410)

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

- **MAIN** 长按约 2.5s 开 SoftAP；短按本身无动作（只会取消 / 重新布防 CYCLE 窗口），见 `button.c`
- **PAGE** 翻页拨杆（同一 GPIO）— 长按约 2.5s 开 / 关 BLE CT-Prov
- **MAIN + PAGE** 同时按住约 6s 恢复出厂（双键需全程按住）
- **芯片 BOOT** GPIO9 — 启动模式选择脚。开机约 50ms 采样一次：低电平 → 检修用恢复出厂。复位全程按住会进入 ROM 下载模式
- 硬件 **RESET** — EN/RST

## 电源 / 指示灯

- 电量：GPIO4 ADC，AW9523B 使能采样
- 状态 RGB：AW9523B；红色脉冲表示故障 / OTA 失败（不会常亮）
