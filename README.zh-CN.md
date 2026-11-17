# EK410

**CamThink** 7.09″ Spectra 6 电子墨水屏固件（ESP32-C61）。

[English](README.md) · [变更记录](CHANGELOG.md) · [文档](docs/) · [许可](LICENSE)

| | |
|---|---|
| **产品** | EK410 · CamThink 电子墨水屏 |
| **主控** | ESP32-C61HR2（16MB Flash，2MB PSRAM） |
| **面板** | 7.09″ Spectra 6 · 1600×1200 · 六色 |
| **固件版本** | 见 [`VERSION`](VERSION) · 当前 **1.3.0** |
| **平台** | [NeoMind](https://wiki.camthink.ai) · 设备类型 `ek410_Eink` |

---

## 功能概览

- **SoftAP 配网** — 长按 **MAIN** 约 2.5s → SoftAP `EK410-{deviceId}` / 密码 `12345678` → `http://192.168.4.1`
- **BLE CT-Prov** — 长按 **PAGE** 约 2.5s → 青色；NeoLink BLE 使用独立 `ek410_ble` OTA 镜像（[docs/ct_prov.md](docs/ct_prov.md)）
- **Web 控制台** — Wi‑Fi、MQTT、图片 / 画布 / Markdown、电源、OTA
- **显示** — 文字、画布、整帧上传、HTTPS URL；上次画面写入 Flash
- **NeoMind MQTT** — `device/ek410_Eink/{deviceId}/uplink|downlink`
- **OTA** — 双 4MB 分区（SoftAP SKU + BLE SKU）+ bootloader 回滚
- **电源** — 常开 / 定时刷新（`sleepMin` + `wakeSec`）/ 休眠（MAIN 唤醒）

### SoftAP 推图流程

1. 本地上传 **或** 填写 URL → 设备 `proxyImage`（走 STA）→ **内容预览**
2. 调整适应方式 / 六色预览 → **显示到屏幕** → 浏览器量化 → `uploadFrame`

MQTT / 唤醒 URL 仍使用设备侧 `displayUrl`。

### 双 SKU 烧录

```bash
bash tools/build_dual_sku.sh
bash tools/flash_dual_sku.sh PORT   # SoftAP → ota_0，BLE → ota_1
```

---

## 快速开始

```bash
export IDF_PATH=~/esp/esp-idf-5.5
export IDF_PYTHON_ENV_PATH=~/.espressif/python_env/idf5.5_py3.11_env
export PATH="$HOME/.espressif/tools/riscv32-esp-elf/esp-14.2.0_20260121/riscv32-esp-elf/bin:$IDF_PYTHON_ENV_PATH/bin:$PATH"

python3 "$IDF_PATH/tools/idf.py" build
python3 "$IDF_PATH/tools/idf.py" -p PORT flash monitor
```

详见 [docs/zh/development.md](docs/zh/development.md)。

---

## 仓库结构

```
EK410/
├── VERSION / LICENSE / CHANGELOG.md
├── CMakeLists.txt
├── partitions.csv          # NVS + 双 OTA + lastfrm
├── sdkconfig.defaults
├── sdkconfig.ble_prov      # BLE SKU 叠加配置
├── main/                   # 固件源码
│   ├── version.h
│   ├── www/                # SoftAP 页面源文件
│   └── …
├── tools/                  # 双 SKU 构建 / CT-Prov 测试等
├── assets/brand|splash/    # 品牌与开机资源
└── docs/                   # 硬件 / API / CT-Prov / 开发
```

---

## 版本管理

遵循语义化版本（`MAJOR.MINOR.PATCH`）。

| 文件 | 作用 |
|------|------|
| [`VERSION`](VERSION) | 权威版本号 |
| [`main/version.h`](main/version.h) | 固件内嵌（设备信息 / MQTT） |
| [`CHANGELOG.md`](CHANGELOG.md) | 发布说明 |
| Git tag | `v1.3.0` |

```bash
python3 tools/bump_version.py --check
python3 tools/bump_version.py 1.3.0
git tag -a v1.3.0 -m "EK410 v1.3.0"
```

完整约定见 [docs/zh/development.md](docs/zh/development.md#版本管理)。

---

## 文档索引

| 内容 | 中文 | English |
|------|------|---------|
| 硬件 | [docs/zh/hardware.md](docs/zh/hardware.md) | [docs/hardware.md](docs/hardware.md) |
| HTTP API | [docs/zh/api.md](docs/zh/api.md) | [docs/api.md](docs/api.md) |
| CT-Prov / NeoLink | — | [docs/ct_prov.md](docs/ct_prov.md) |
| 开发 | [docs/zh/development.md](docs/zh/development.md) | [docs/development.md](docs/development.md) |
| 工具 | [tools/README.md](tools/README.md) | — |
| UI 设计预览 | [docs/design/ui-preview/DESIGN.md](docs/design/ui-preview/DESIGN.md) | — |

---

## NeoMind

配置好 Wi‑Fi 与 MQTT 后，设备会出现在 NeoMind → 设备（待审批）。审批后即可用规则 / Agent 推送内容上屏。

设备类型定义在 **NeoMind-DeviceTypes** 仓库（`ek410_Eink`）。

---

## 许可

双许可证（社区版 + 商业版），条款与 [camthink-ai/ne301](https://github.com/camthink-ai/ne301) 一致。详见 [`LICENSE`](LICENSE)。
