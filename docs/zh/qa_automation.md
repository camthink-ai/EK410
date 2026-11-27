# EK410 单固件自动化测试方案

源码：[github.com/camthink-ai/EK410](https://github.com/camthink-ai/EK410)

**发布验收使用同一份 `ek410.bin`。** 用这份镜像验证 SoftAP、BLE、MQTT、刷屏、睡眠和 OTA。三层门禁：

| 门禁 | 入口 | 用途 |
|------|------|------|
| 基础回归 | `python3 tools/qa_gate.py --tier smoke` | 每次改动 |
| 候选压力 | `python3 tools/qa_gate.py --tier candidate` | 进发布前真机 |
| 72h 稳定 | `python3 tools/qa_gate.py --tier soak --hours 72` | 最终候选 |

长期稳定用可观测指标验收：**零异常重启、零刷屏中途切电、零静默丢命令**，以及内存、功耗和电池预算达标。下表循环次数为**首版建议门槛**，功耗绝对值需结合硬件与续航目标再定。

机器可读结果由 `qa_gate.py` 写出 JSON：每条用例 `status` 为 `pass` / `fail_device` / `fail_fixture` / `skipped` / `invalid`（硬件断联或测量缺失 → `invalid`，不算通过也不算设备失败）。

两条门禁规则：

- **报告绑定被测镜像**：`firmware_binding` 用例比对设备上报的 `fwVersion` 与镜像内嵌版本，不一致记 `fail_device`，报告无法为设备没在跑的固件背书。
- **全跳过不算通过**：一条 `pass` 都没有的运行 `ok=false`（退出码 1），即使没有任何失败。

设备不再通过 HTTP 回传 MQTT 密码（`getMqttParam` 只给 `hasPassword`）。改 broker / 下行发布相关用例需要显式传 `--mqtt-password`，否则这些用例记为跳过而不是失败。

主机回归测试（无需设备）：`python3 tools/run_host_tests.py`，覆盖 JSON 嵌套深度防护、整屏抖动内核（`main/img_dither.c` 在主机上编译：阶调、灰色不出彩色、与 `frame_convert.py` 逐字节一致的帧布局）、原生帧布局、QA 门禁语义、内嵌网页与源码同步、字库表；`build_release.sh` 构建前会自动执行。

## 修复核对（candidate 档）

`candidate` 档还会执行 `tools/fix_verification.py`：逐项在设备上核对本版的修复，旧固件上会报失败——深层 JSON 被拒绝、`devName` 长度限制、MQTT 密码不回传、URL 升级失败通过 `otaState` 可见**且 MQTT 随后恢复**、用待发布镜像做一次本地上传 OTA 往返。加 `--verify-url`（设备能访问的图片地址）可跑 URL 刷图压力，同时核对每轮之后 MQTT 恢复、PSRAM 不逐轮减少；加 `--verify-raw-url`（960000 字节原生帧地址）核对原生帧夹在普通图片之间也能显示——这是画质最好的路径，需要一整块约 940 KB 的 PSRAM；`--power` 额外核对 `sleepMin` 上限。`GET /api/v1/system/getDiag` 带有 `heap` 与 `stackMin`（各任务最小剩余栈，字节），每轮压力前后都读一下。ESP32-C61 在 Wi-Fi、网页服务、MQTT 都起来的稳态下，`heap.freeInt` 应保持在约 15 KB 以上，`largestInt` 在 8 KB 以上（低于 8 KB 时 MQTT 起不来）。

```bash
python3 tools/qa_gate.py --tier candidate --port PORT --host IP --mqtt-password '…' --verify-url http://PC:8000/t.png --verify-raw-url http://PC:8000/frame.bin
python3 tools/fix_verification.py --host IP --url http://PC:8000/t.png --rounds 20 --raw-url http://PC:8000/frame.bin --firmware dist/ek410.bin
```

## 业务节奏长测（72 小时）

`tools/soak_long.py` 是 candidate 档的无人值守补充：一台设备、平时的业务流量、连续跑若干小时，并且从设备的两侧看它——HTTP 这侧，以及带 `--broker-secret` 时它所上报的 broker 那侧。脚本在本机起图片服务（自动生成测试图：横幅 / 竖幅 / 长条 / 620 KB 照片四种 JPEG、灰度 JPEG、真彩 / 六色调色板 / 灰度 / 带透明通道四类 PNG、正好 700 KB 的边界文件、原生帧、跳转、分块传输、无长度传输），并模拟服务器的几种毛病（HTTP 404、传一半断开、卡住不动、自签名证书的 HTTPS 服务），每一种都必须以固件日志里的原因失败。图片之间穿插看板画布、长文本屏、`uploadFrame` 原生帧、固件必须拒绝的文件、网页控制台，带 `--https-url` 时一张公网 HTTPS 图片，以及每隔 `--ota-every-hours` 把同一份固件再上传一次（设备必须回到另一个 OTA 槽）。刷图片的那几轮会在刷屏期间发起控制台用户的其它请求（状态轮询和网页、`proxyImage`、第二个 `displayUrl`，以及必须被立刻拒绝的 `uploadFrame`），每轮之后读回设备保存的帧（`GET /api/v1/display/lastFrame`）：上传帧和原生帧必须与发出的逐字节一致，画布和文本必须与第一次相同，每次刷屏都必须存下了自己的帧。每轮之后读 `getDiag`（堆、各任务最小剩余栈）、确认 MQTT 已恢复。带 `--broker-secret`（见 `tools/broker_secret.py`）时，在设备的 broker 上再开一个客户端：确认每轮之后到达的遥测内容属实（帧行的 CRC、预览图尺寸），每隔 `--mqtt-every` 轮把命令改从 broker 下发（先回 `queued`，刷完回 `ok` 或带原因的 `error`），每隔 `--ping-every` 轮核对 ping 有应答。运行期间如果有人在设备上打开了蓝牙配网或热点（按键、App），该轮不判定；判定的是：之后设备重新成为客户端。

```bash
python3 tools/soak_long.py --host IP --out ~/ek410-soak --port PORT --hours 72 --image dist/ek410.bin \
    --https-url https://HOST/photo.jpg --https-every 37 --ota-every-hours 24 --broker-secret ~/.ek410-broker.json
python3 tools/soak_long.py --host IP --rounds 2 --no-caffeinate     # 工具自检
```

只有同时满足以下条件才算 `ok`（退出码 0）：没有意外重启、每次刷屏都是预期结果、没有被拒绝的请求、每次刷屏后 MQTT 恢复且不会整段间隔都离线、设备始终可达、并发请求下没有异常、读回的帧与发出的一致且不空白、每次计划内升级都完成、串口没有故障和缺陷标记；带 `--broker-secret` 时还要求：broker 收到了每条命令的结果、每轮之后有属实的遥测、ping 有应答、任何配网之后设备重新成为客户端。运行期间主机不能休眠（macOS 下脚本会自动转到 `caffeinate -i -s` 下运行；合盖仍会让笔记本休眠——睡过去的轮次只记录、不判定），也不要有别的程序打开串口或给设备烧录。`touch <out>/STOP` 可提前结束。`tools/tests/test_soak_dryrun.py`（在 `run_host_tests.py` 里）用模拟的设备和 broker 把整个工具跑一遍，并确认它能注入的每一种故障都会被判出来。

---

## 1. 测试台和数据记录

建议至少两块 EK410，烧录相同 SHA-256 固件。一块跑业务与配网切换，另一块接电源分析仪跑休眠与唤醒。台架：UART、可控 Wi‑Fi AP、本地 MQTT broker、可控图片服务、可编程电源、按键夹具。条件允许时同步采面板 `BUSY` 与供电使能。

每条测例至少记录：固件 SHA、设备编号、供电电压、时间、命令 ID、重启/唤醒原因、MQTT／HTTP 状态、结果。内存按能力记 **内部 RAM、PSRAM、DMA 的最大连续块与最低可用量**，并采渲染／MQTT／HTTP 等栈余量。生产功耗用最终发布配置测，避免高频调试输出改变休眠。

复用：

- `tools/release_smoke.py` — 身份 / CT-Prov / XOR / MQTT 忙与配置一致性 / CA 重启读回  
- `tools/ct_prov_xor_test.py` — SoftAP ↔ BLE 互斥冒烟  
- `tools/stability_stress.py` — 边界与压力  
- `tools/qa_gate.py` — 统一入口与报告  
- `tools/qa_lib/` — 结果模型、固件 SHA、功耗夹具桩  

---

## 2. 真机自动化验收矩阵

| 测试项 | 方法 | 建议门槛 | 当前落地 |
|--------|------|----------|----------|
| 单固件启动与配置保留 | 反复启动、配置后断电重启 | 20 次成功；除恢复出厂外读回一致；零 panic/WDT/意外回滚 | smoke 含 boot + CA 重启读回；循环待加 |
| SoftAP ↔ BLE 配网 | 反复切换、错密、空闲退出、恢复 STA | 30 轮；互斥；恢复窗口内 STA/HTTP/MQTT | smoke XOR + xor 工具；candidate 可加 `--xor-rounds` |
| MQTT 忙时配网 | 大预览上报同时进 BLE/SoftAP | 各 10 次；忙可观察；遥测保持；解除后可再切 | `release_smoke` 已覆盖一轮竞态 |
| MQTT 配置一致性 | 忙时改 broker/禁用；正常改 + 重启读回；CA | 忙失败保持旧配置；成功后读回一致；CA 失败如实报错 | smoke 已覆盖 |
| 网络故障与恢复 | 断 AP/broker/DNS、慢响应、TLS 失败 | 每类 20 次；有界重试；记 P50/P95 | 待台架 |
| 刷屏与休眠互斥 | BUSY 各阶段触发休眠 | ≥30 次重叠；刷屏时面板供电保持 | 待电源/BUSY 采集 |
| 图片、预览与 OOM | 合法/非法图、并发预览与遥测 | ≥100 刷新 + 20 失败注入 | stress 部分覆盖 |
| Web 与实际状态 | 慢客户端、重复点击、关闭 SoftAP 时查询 | 队列满/超时可见；接收≠完成 | 部分在 stress |
| OTA 与掉电 | 旧分区表 OTA、回滚、缺 evtlog、分阶段断电 | 旧机可更新；提示与数据一致 | 待专项 |

**OOM**：失败后须能再下载、显示、休眠。同空闲态比较循环前后内存，勿把首次缓存当泄漏。

---

## 3. 功耗自动化验收

同时验深睡电流、每次唤醒耗电、误唤醒次数。

| 指标 | 测法 | 判定 |
|------|------|------|
| 深睡稳态电流 | 产品供电路径，稳态窗口 | P95 ≤ 硬件上限；相对基线建议 >10% 回退复核 |
| 休眠时机 | 定时睡 / 按键醒 | 正常必入睡；刷屏忙时保持供电 |
| 唤醒准确性 | 100 次定时 + 30 次按键 | 零漏醒、零非预期醒 |
| 单次业务耗电 | 上报 / 刷屏 / 断网恢复积分 | 相对基线 >10% 先分段定位 |
| 网络异常续航 | AP/broker 不可用完整周期 | 有预算重试后回睡 |

> 每日耗电 ≈ 深睡电流×深睡小时 + Σ(单次耗电×每日次数)  
> 续航天数 ≈ 电池可用容量 ÷ 每日耗电  

至少三套场景：正常定时、频繁刷屏、网络不稳。无正式续航目标前，**不填绝对 μA / 天数硬门槛**。

`qa_lib.power` 提供夹具接口；未配置仪表时该项记 `invalid` / `skipped`。

---

## 4. 长期稳定性与发布判定

- **每次候选**：`--tier smoke`（约数十分钟内，视网络而定）— SHA、双配网路径、MQTT、关键一致性。  
- **发布前**：两台设备各 `--tier soak --hours 72`（一台业务节奏，一台断网/繁忙/坏图；压力机仍须有完整睡眠窗）。  

72h 结束发布记录须能回答：稳定性（零 panic/WDT/非计划重启）、业务结果、资源趋势、功耗与刷屏零切电、可追溯（SHA、串口、波形、汇总、失败样本）。

**落地顺序**：睡眠电流与面板切电采集 → MQTT 忙时自动化（已基本具备）→ 72h 长测。本文为首版执行方案，循环次数与 10% 回退线尚未用 EK410 真机数据校准。

**与固件产品闭环**：`mqttReason` / `provError` / `displayBusy` / `sleepDeferred`、SoftAP·BLE Web 入口、端口↔协议告警已在设备侧收口；`qa_gate.py smoke` 不替代 candidate/soak 真机门禁（无 CI、功耗仪表与断电循环用例仍多为 skipped/invalid）。
