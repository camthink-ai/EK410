# 开发说明

英文版：[development.md](../development.md)

## 环境

- ESP-IDF **5.5.x**（组件要求 `>=5.3.0`）
- 芯片目标 `esp32c61`
- 建议使用 IDF 自带的 Python 3.11 虚拟环境

## 编译烧录

```bash
export IDF_PATH=~/esp/esp-idf-5.5
export IDF_PYTHON_ENV_PATH=~/.espressif/python_env/idf5.5_py3.11_env
export PATH="$HOME/.espressif/tools/riscv32-esp-elf/esp-14.2.0_20260121/riscv32-esp-elf/bin:$IDF_PYTHON_ENV_PATH/bin:$PATH"

cd /path/to/EK410
python3 "$IDF_PATH/tools/idf.py" build
python3 "$IDF_PATH/tools/idf.py" -p /dev/cu.wchusbserialXXXX flash monitor
```

`sdkconfig` 不入库，以 `sdkconfig.defaults` 为准。

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
python3 tools/bump_version.py 1.3.0
# 填写 CHANGELOG → commit →
git tag -a v1.3.0 -m "EK410 v1.3.0"
```

- **MAJOR** 破坏协议；**MINOR** 兼容新功能；**PATCH** 修复
- Tag 格式：`v1.2.1`
- 勿轻易改 `deviceId` 生成逻辑（NeoMind 身份）
