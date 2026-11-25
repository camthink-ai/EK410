# 开发说明

英文版：[development.md](../development.md)

产品 Wiki 上的编译 / 烧录说明：[CamThink Wiki · 开发环境](https://wiki.camthink.ai/zh-Hans/docs/epaper-ek410-series/software-guide/ek410-development-environment) · [English](https://wiki.camthink.ai/docs/epaper-ek410-series/software-guide/ek410-development-environment)

仓库：[https://github.com/camthink-ai/EK410](https://github.com/camthink-ai/EK410)

## 环境

- ESP-IDF **5.5.x**（组件要求 `>=5.3.0`）
- 芯片目标 `esp32c61`
- 建议使用 IDF 自带的 Python 3.11 虚拟环境

## 克隆

```bash
git clone https://github.com/camthink-ai/EK410.git
cd EK410
```

## 编译烧录

```bash
. $HOME/esp/esp-idf-5.5/export.sh
cd /path/to/EK410

idf.py set-target esp32c61          # 仅首次
idf.py build
idf.py -p /dev/cu.wchusbserialXXXX flash monitor
```

### 便捷脚本（单镜像）

```bash
bash tools/build_release.sh
bash tools/flash_release.sh /dev/cu.wchusbserialXXXX
python3 tools/ct_prov_xor_test.py --port /dev/cu.wchusbserialXXXX
```

一套 `sdkconfig.defaults` → 一份 `ek410.bin`。SoftAP 与 BLE CT-Prov 同镜像，
运行时互斥。`sdkconfig` 不入库。

## 配置页

改 `main/www/index.html` 后执行：

```bash
python3 tools/gen_www.py
```

## 版本管理

| 文件 | 作用 |
|------|------|
| `VERSION` | 权威 SemVer |
| `main/version.h` | 固件内版本宏 |
| `CHANGELOG.md` | 变更记录 |
| `tools/bump_version.py` | 同步版本号 |

```bash
python3 tools/bump_version.py --check
python3 tools/bump_version.py X.Y.Z
# 填写 CHANGELOG → commit →
git tag -a vX.Y.Z -m "EK410 vX.Y.Z"
```

- **MAJOR** 破坏协议；**MINOR** 兼容新功能；**PATCH** 修复
- Tag 格式：`vX.Y.Z`（如 `v1.3.10`）
- 勿轻易改 `deviceId` 生成逻辑（NeoMind 身份）

## 工具

见 [tools/README.md](../../tools/README.md)。
