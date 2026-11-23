# EK410

[![Platform](https://img.shields.io/badge/platform-ESP32--C61-blue)]()
[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-5.5-orange)]()
[![Version](https://img.shields.io/badge/version-1.3.4-blue)](VERSION)
[![License](https://img.shields.io/badge/license-Dual--License-red)](LICENSE)

**CamThink** 7.09″ Spectra 6 电子墨水屏固件。

[English](README.md) · [变更记录](CHANGELOG.md) · [文档](docs/) · [许可](LICENSE)

| | |
|---|---|
| **产品** | EK410 · CamThink 电子墨水屏 |
| **主控** | ESP32-C61HR2 · 16MB Flash · 2MB PSRAM |
| **面板** | Spectra 6 · 逻辑 1600×1200 · 六色 |
| **固件** | [`VERSION`](VERSION) → **1.3.4** |
| **平台** | [NeoMind](https://wiki.camthink.ai) · 设备类型 `ek410_Eink` |

---

## 概览

EK410 是偏电池场景的墨水屏终端：SoftAP / BLE 配网、内嵌 Web、NeoMind MQTT；双 OTA 槽用于升级回滚。SoftAP 与 BLE CT-Prov 共用 **同一固件**（进程内互斥）。断电后面板保持最后一帧；在线时 Wi‑Fi / MQTT 会自动重连。

```
┌────────────────────────────────────────────────────────────┐
│  SoftAP 网页  ·  NeoLinker CT-Prov  ·  NeoMind MQTT / HTTP │
├────────────────────────────────────────────────────────────┤
│  单镜像 ek410 — MAIN SoftAP（蓝）XOR PAGE BLE（青）        │
│  ota_0 / ota_1 = 升级回滚（同一二进制）                     │
├────────────────────────────────────────────────────────────┤
│  Spectra 6 面板 · lastfrm · AW9523B RGB / 电源             │
└────────────────────────────────────────────────────────────┘
```

详见 [docs/ct_prov.md](docs/ct_prov.md)。

---

## 功能

- **SoftAP + Web** — Wi‑Fi / MQTT / 图片 / 画布 / Markdown / 电源 / OTA
- **[NeoLinker](https://github.com/camthink-ai/NeoLinker) CT-Prov** — SoftAP HTTP + BLE GATT（单镜像，SoftAP XOR BLE）
- **显示** — 整帧上传、URL 拉取、画布、Markdown；上次画面存 `lastfrm`
- **NeoMind MQTT** — `device/ek410_Eink/{deviceId}/uplink|downlink`
- **电源** — 常开 / 定时刷新（`sleepMin` + `wakeSec`）/ 休眠；深睡关灯与面板供电
- **可靠性** — Wi‑Fi 指数退避重连；MQTT 自动重连；硬件 RESET 退出配网并恢复上次画面（无内容则 CamThink logo）

### SoftAP 推图

1. 本地上传 **或** URL → 设备 `proxyImage`（STA）→ 内容预览  
2. 适应方式 / 六色预览 → **推屏** → 浏览器量化 → `uploadFrame`  

MQTT / 唤醒 URL 仍走设备侧 `displayUrl`。

---

## 按键与指示灯

| 操作 | 行为 |
|------|------|
| **MAIN** 长按约 2.5s | 开 SoftAP（`EK410_<MAC6>` / `12345678` → `http://192.168.4.1`）· 蓝 |
| **PAGE** 长按约 2.5s | 进入 / 退出 BLE CT-Prov · 青 |
| **MAIN+PAGE** 长按约 6s | 恢复出厂（保留 device id） |
| 深睡 **MAIN** | 唤醒 → SoftAP |
| 深睡 **PAGE** | 唤醒 → BLE |
| 深睡定时器 | cycle 唤醒（可选唤醒图 + 在线窗） |
| 硬件 **RESET** | 退出 SoftAP/BLE handoff · 恢复上次画面 |
| 芯片 **BOOT**（IO9）复位时按住 | 恢复出厂 |

| 灯色 | 含义 |
|------|------|
| 蓝闪 | SoftAP / 正在连 Wi‑Fi |
| 青闪 | BLE 配网 |
| 绿灯常亮 | 正常运行（STA 已连接） |
| 深睡灭灯 | LED 与面板供电已关闭 |

SoftAP / BLE 空闲自动关闭：**2 分钟**（有操作会续期）。

---

## 电源模式

| 模式 | 行为 |
|------|------|
| `always` | 常醒；`sleepNow` → 深睡至 MAIN |
| `cycle` | 醒来后在线 `wakeSec` 秒（30–600，默认 **120**），再睡 `sleepMin` 分钟 |
| `hibernate` | 睡到 MAIN（保存参数时可能立刻睡） |

---

## 快速开始

**环境：** ESP-IDF **5.5.x**，目标 `esp32c61`。

```bash
. $HOME/esp/esp-idf-5.5/export.sh
cd /path/to/EK410

idf.py set-target esp32c61          # 仅首次
idf.py build
idf.py -p PORT flash monitor
```

### 便捷脚本

```bash
bash tools/build_release.sh        # fullclean + build → dist/ek410.bin
bash tools/flash_release.sh PORT    # 烧 bootloader + 分区表 + 镜像（双槽同镜像）
```

更多：[docs/zh/development.md](docs/zh/development.md) · [tools/README.md](tools/README.md)。

---

## 仓库结构

```
EK410/
├── VERSION · LICENSE · CHANGELOG.md
├── CMakeLists.txt · partitions.csv · sdkconfig.defaults
├── main/                 # 固件（wifi / mqtt / http / ct_prov / app_epd / www/…）
├── tools/                # gen_www、bump_version、构建/烧录辅助
├── assets/               # 品牌与开机资源
└── docs/                 # 硬件、API、CT-Prov、开发
```

`build/`、`dist/`、`sdkconfig`、`managed_components/` 已加入 `.gitignore`。

---

## 版本管理

| 文件 | 作用 |
|------|------|
| [`VERSION`](VERSION) | 权威版本号 |
| [`main/version.h`](main/version.h) | 固件内嵌 |
| [`CHANGELOG.md`](CHANGELOG.md) | 发布说明 |
| Git tag | `v1.3.4` |

```bash
python3 tools/bump_version.py --check
python3 tools/bump_version.py 1.3.1
git tag -a v1.3.1 -m "EK410 v1.3.1"
```

---

## 文档索引

| 内容 | 中文 | English |
|------|------|---------|
| 硬件 | [docs/zh/hardware.md](docs/zh/hardware.md) | [docs/hardware.md](docs/hardware.md) |
| HTTP API | [docs/zh/api.md](docs/zh/api.md) | [docs/api.md](docs/api.md) |
| CT-Prov / [NeoLinker](https://github.com/camthink-ai/NeoLinker) | — | [docs/ct_prov.md](docs/ct_prov.md) |
| 开发 | [docs/zh/development.md](docs/zh/development.md) | [docs/development.md](docs/development.md) |
| 工具 | [tools/README.md](tools/README.md) | — |

---

## NeoMind

配好 Wi‑Fi + MQTT 后，设备出现在 NeoMind → 设备（待审批）。设备类型：**NeoMind-DeviceTypes** / `ek410_Eink`。

---

## 许可

**双许可证**（社区版 + 商业版），与 [camthink-ai/ne301](https://github.com/camthink-ai/ne301) 同模式。见 [`LICENSE`](LICENSE)。
