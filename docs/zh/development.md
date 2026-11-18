# 开发说明

英文版：[development.md](../development.md)

## 环境

- ESP-IDF **5.5.x**（组件要求 `>=5.3.0`）
- 芯片目标 `esp32c61`
- 建议使用 IDF 自带的 Python 3.11 虚拟环境

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
bash tools/build_dual_sku.sh
bash tools/flash_dual_sku.sh /dev/cu.wchusbserialXXXX
```

`sdkconfig` 不入库；默认（含 NimBLE）见 `sdkconfig.defaults`。

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
python3 tools/bump_version.py 1.3.1
# 填写 CHANGELOG → commit →
git tag -a v1.3.1 -m "EK410 v1.3.1"
```

- **MAJOR** 破坏协议；**MINOR** 兼容新功能；**PATCH** 修复
- Tag 格式：`v1.3.0`
- 勿轻易改 `deviceId` 生成逻辑（NeoMind 身份）

## 工具

见 [tools/README.md](../../tools/README.md)。
