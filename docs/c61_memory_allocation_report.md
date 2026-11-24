# EK410 / ESP32-C61 资源分配困难点报告

**Audience:** 其它 AI / 固件评审（可独立阅读）  
**Product:** CamThink EK410（7.09″ Spectra 6 墨水屏边缘设备）  
**SoC:** ESP32-C61（单核 RISC-V + **~128KB 量级内部 SRAM** + **2MB PSRAM**）  
**FW context:** SoftAP XOR BLE 单镜像；STA + SoftAP(APSTA) + httpd + MQTT/MQTTS + 墨水屏刷屏  
**Date:** 2026-11-19  
**Repo commits (related):** `b59eed1`（LAN Web OOM 重启）、`7b87289`（产品闭环）、`9aeaff1`（SoftAP+MQTT 共存）

---

## 1. 一句话结论

**瓶颈不是总堆（PSRAM 有约 2MB），而是「内部连续空闲块 `largest_int`」。**  
FreeRTOS 任务栈、httpd worker、Wi‑Fi 部分缓冲、lwIP/DNS semaphore、`esp_timer`、PHY 相关结构必须落在 **internal RAM**。SoftAP + httpd + MQTT 同时常驻时，`largest_int` 经常掉到 **4–8KB**，低于 MQTT TCP 栈（3KB+余量）或 TLS 栈（8KB）与后续 DNS/PHY 分配，表现为：

- `wifi:mem fail` / `httpd_accept (113)` / `otadata mmap NO_MEM`
- `lwip_arch: thread_sem_init: out of memory`
- `phy_track_pll_init` 里 `ESP_ERROR_CHECK(ESP_ERR_NO_MEM)` → **abort 整机重启**
- 用户侧：打开设备 IP 网页就重启；SoftAP 配网时测 MQTT 失败/卡住；UI「未连接」但云端有历史数据

---

## 2. 硬件与堆模型

| 资源 | 约略规模 | 谁能用 |
|------|----------|--------|
| Internal SRAM | 启动日志常见 **~114KiB + ~11KiB** 动态区 | 任务栈、DMA 敏感、Wi‑Fi/部分驱动、小对象 |
| PSRAM | **2MB** @ 80MHz | `CONFIG_SPIRAM_USE_MALLOC`；≥1KB 优先外置（`ALWAYSINTERNAL=1024`） |
| Flash | 16MB；SPA gzip 嵌入 rodata | 首页线传约 **63–66KB** gzip（原 HTML ~230KB） |

关键度量（串口常打）：

```text
heap_caps_get_free_size(MALLOC_CAP_INTERNAL)     // int free
heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)  // largest_int  ← 真正卡点
esp_get_free_heap_size()                         // 含 PSRAM，易误导
```

**经验门槛（非安全证明）：** MQTT **client_start 前**要求 `largest_int ≥ 8192`（`mqtt_internal_need()`；本文初稿写的是 10240，现行代码取 8 KB——开机时 httpd 已起、最大的块约 8.7 KB，10 KB 会让它永远 defer）。这只覆盖启动瞬间；DNS/TCP/TLS 握手高峰是独立指标，尤其 MQTTS。低于门槛强起曾触发 `phy_track_pll` NO_MEM abort。

> 2026-11-24 真机实测（1.3.10）说明「8 KB 门槛」只是勉强过线：开机时最大的块 8704 字节，**门槛之上只剩 512 字节**。MQTT 第一次停止后最大的块只剩 4096，此后一直 `deferred_heap`。根治办法不是调门槛，而是把内部 RAM 预算腾出来——见第 11 节。

### SoftAP「测试 MQTT」时序（已按评审修）

网页曾在 `setMqttParam` 之后立刻 `testMqtt`，而 SoftAP 下 bringup 会 pause httpd → 与同步 `testMqtt` 死锁/误报。现行约定：

1. `setMqttParam` **先发 HTTP 响应**，再 `mqtt_start()`（短 delay 冲刷 TCP）
2. `mqttDeferred` → 前端结束，**不**发 `testMqtt`
3. `mqttSoftApConnect` / `mqttSoftApSession` → 有界轮询 `getDevInfo`（允许短暂断页重试），**不**再 POST `testMqtt`
4. SoftAP 下 save 回包丢失时同样只轮询 `getDevInfo`，**禁止**边缘回退到 `testMqtt`
5. `wifi_httpd_pause_for_mqtt()` 返回是否真正停住；停不完则 defer，不假称已 pause

---

## 3. 并发服务的内部 RAM 预算（定性）

以下为「必须抢 internal」的大头，非精确 profiler：

| 组件 | Internal 压力 | 备注 |
|------|----------------|------|
| Wi‑Fi STA / SoftAP beacon | 高 | 默认 buffer 过大时 `largest_int≈3.7–4KB`；现默认 **lean buffers** |
| httpd | 高 | **一个** server task（栈 **4608–5120**）服务多个连接；`max_open_sockets` 主要削减 socket/lwIP，不是「每连接一份栈」。SoftAP 期 sockets=**2**，STA 期 **3** |
| MQTT client task | 高 | TCP **4096**（1.3.9 为 3072，连接成功后只剩约 170 字节），TLS **8192**；栈不能放 PSRAM（C61 上 PSRAM 栈 create 会坏） |
| `mqtt_cmd` / `mqtt_up` | 中 | 开机创建 5120 / 6144，避免后期 NO_MEM |
| MQTT TX buffer | 主要在 PSRAM | `MQTT_TX_BUF=220KB`（预览遥测）；SoftAP 期 **跳过 frame_preview** |
| 墨水屏 render 任务 | 低 | **16KB 栈**（1.3.9 为 32KB，真机实测最深路径远用不到——见第 11 节）；刷屏时与 MQTT stop 互锁 → `mqtt stop busy` |
| JSON（cJSON） | 低（1.3.10 起） | 节点与键串优先 PSRAM；此前每个节点都走内部 RAM，一次 `getDevInfo` 约 5 KB |
| NimBLE | 互斥 | SoftAP XOR BLE：进 BLE 会 teardown Wi‑Fi/httpd/mqtt |

`sdkconfig.defaults` 要点：

- `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=28672`
- `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y`
- `CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE=3072`（1.3.10 起；IDF 默认 2304 在真机上只剩约 600 字节）
- Wi‑Fi STATIC/DYNAMIC RX/TX 保持 SoftAP 可用量级；运行时 `s_wifiLeanBuffers=true` 再裁一刀

---

## 4. 已观测故障链（根因级）

### 4.1 打开 LAN IP `http://192.168.x.x/` 设备重启

1. 一次性推送 ~212–230KB 未压缩 SPA，或并发 API + MQTT  
2. `W wifi:mem fail` → `httpd_accept: error in accept (113)`  
3. 后续 `mmap otadata` / 其它 `NO_MEM`  
4. 或 GOT_IP 后 MQTT 在 `largest_int≈4352` 仍启动 → DNS/PHY NO_MEM → **abort**

**缓解：**

- 首页 **预 gzip + chunked**（线传 ~63KB）
- STA 未 online 时不强启 httpd（避免失败 join 后 `ESP_ERR_HTTPD_TASK`）
- MQTT 启动门槛 `largest_int≥10KB`
- 冷启动 lean Wi‑Fi buffers

### 4.2 SoftAP 配网时「想连上 MQTT」

产品需求：客户在 SoftAP 页完成 **家里 Wi‑Fi + MQTT**，关热点前就要看到 **已连接**。

硬约束：SoftAP beacon + httpd workers + MQTT task 三者同时吃满 internal。

**错误做法（已踩坑）：** SoftAP 期完全禁止 MQTT → 体验「配置了但测不通/永远未连接」。  
**另一错误做法：** SoftAP+STA 时直接 `mqtt_start` 不腾堆 → OOM/重启。

**现行做法（`9aeaff1`）：**

```text
STA 已连（APSTA）且 SoftAP up
  → mqtt_bringup:
       wifi_httpd_pause_for_mqtt()   // SoftAP 射频保留，只停 httpd
       wait largest_int 够用
       esp_mqtt_client_start(...)
       wifi_httpd_resume_after_mqtt() // SoftAP 用 lean：2 sockets / 4608 stack
  → SoftAP 期遥测不带 frame_preview_b64
```

副作用：连接瞬间网页 **1–2s 无响应**（手机仍挂在 SoftAP SSID 上）。

### 4.3 修改 MQTT 配置是否生效

路径与首次连接相同（`setMqttParam`）：

1. 配置有变 → `mqtt_stop_wait`  
2. 写 NVS（先 CA 再 broker）  
3. `mqtt_start()` → SoftAP 下同样走 pause/resume  

**失效条件：**

- `mqtt stop busy`（大预览遥测占 mutex）→ **不落盘**，返回错误  
- SoftAP 下未连家里 Wi‑Fi → 只保存，无法连公网 Broker  
- 空密码 = 不改密码（避免浏览器空白覆盖 NVS）  
- `keep_live` 仅在 host/port/qos/auth/tlsVerify/transport 与 CA 均未变且已在线时跳过 stop/restart（`same_broker` 已含 `qos`）

### 4.4 SoftAP ↔ BLE XOR

进 SoftAP/BLE 前必须 `mqtt_stop_wait`。超时则 **abort 进入**，`provError=mqtt_busy`，Web 需提示稍后重试。  
BLE 与 SoftAP **不能**同时 up（单镜像互斥）。

---

## 5. 当前策略总表

| 场景 | 策略 |
|------|------|
| 服务大 SPA | gzip + 2KB chunk + yield |
| 冷启动 Wi‑Fi | lean buffers 默认 on |
| MQTT 启动门 | `largest_int ≥ 8192`；异步 `mqtt_up` 任务（勿在 sys_evt 里做重活） |
| SoftAP + MQTT | pause httpd → MQTT → lean httpd；遥测无 preview |
| SoftAP 无 STA | 允许写 NVS；`mqttReason=softap_need_sta` |
| SoftAP 关 | `wifi_network_services_restore`：httpd 优先再 MQTT |
| BLE | teardown Wi‑Fi/httpd/mqtt；堆紧可跳过 Wi‑Fi warm / GATT worker |
| 刷屏 vs MQTT stop | 有界 wait；失败表面化为 busy，禁止半写入新 broker |

---

## 6. 关键代码入口

| 主题 | 文件 / 符号 |
|------|-------------|
| SoftAP MQTT pause/resume | `main/wifi.c`：`wifi_httpd_pause_for_mqtt` / `wifi_httpd_resume_after_mqtt` |
| MQTT bring-up | `main/mqtt.c`：`mqtt_bringup_task` / `internal_heap_ready_for` / `mqtt_esp_start_locked` |
| setMqtt / SoftAP 提示 | `main/http.c`：`set_mqtt_param_handle` |
| Lean httpd | `main/http.c`：`http_server_start`（softap→2 sockets） |
| SPA gzip | `tools/gen_www.py` → `main/www_html.h`；`get_root_handler` |
| 状态字 | `mqtt_status_detail` → `getDevInfo.mqttReason` |

---

## 7. 给其它 AI 的评审问题（开放）

1. **是否有更好的 SoftAP+MQTT 共存**，避免 pause httpd？（例如 httpd 永久 1 worker + MQTT 更小栈；或 SoftAP 仅配网页静态极简 HTML）  
2. **MQTTS（8KB 栈）在 SoftAP+APSTA 下** 是否仍会系统性 defer？如何在不 abort 的前提下给 UI 明确进度？  
3. **`ESP_ERROR_CHECK` in `phy_track_pll_init`** 遇 NO_MEM 直接 abort——除提高门槛外，能否在应用层进一步隔离（减少并行 timer/DNS）？  
4. MQTT `out_size=220KB` 虽走 PSRAM，发布路径是否仍间接抬升 internal 碎片？SoftAP 期是否应强制更小 `buffer.out_size`？  
5. 墨水屏 32KB render 任务与 MQTT stop 争用：是否应用「配网窗口禁止大遥测/禁止刷屏」硬门闩？  
6. ~~`keep_live` 未覆盖 QoS~~：**已关闭** — `set_mqtt_param_handle` 的 `same_broker` 已比较 `qos`；仅改 QoS 会 stop/restart。

---

## 8. 非目标 / 已知不闭环

- 旧分区无 `evtlog`：需全量烧录，无法运行时造分区  
- QA `qa_gate`：无 CI、功耗仪表、断电循环多为 skipped  
- SoftAP **仅** AP、无 STA：物理上无法测公网 MQTT（不是堆问题）  
- BLE 与 SoftAP 同时配网：刻意不做

---

## 9. 复现与观测建议

```bash
# 串口过滤
grep -E 'largest_int|mem fail|httpd_accept|NO_MEM|pause httpd|resume httpd|MQTT: init|abort|Rebooting'

# SoftAP 路径
# 1) 手机连 EK410 SoftAP → 192.168.4.1
# 2) 扫描并连接家里 Wi‑Fi（APSTA）
# 3) 保存/测试 MQTT → 期望短暂断页后 mqttConnected=true
# 4) 改 host 再保存 → 应停旧连新；busy 则报错且 NVS 不变
```

`getDevInfo` 关键字段：`mqttConnected`, `mqttReason`, `mqttStarted`, `softapUp`, `wifiConnected`, `provError`, `displayBusy`。

## 10. 摘要（可贴进其它 AI 的 system 上下文）

> EK410 on ESP32-C61: PSRAM is plentiful but **contiguous internal RAM (`largest_int`)** is the scarce resource. Task stacks (httpd workers, MQTT 3K/8K) cannot live in PSRAM. SoftAP+httpd+MQTT concurrent use previously caused wifi mem fail, accept(113), and phy NO_MEM aborts. Mitigations: gzip SPA, lean Wi‑Fi buffers, MQTT start only if `largest_int≥10KB`, and for SoftAP+STA MQTT: **briefly pause httpd → start MQTT → resume lean httpd (2 sockets)**, skip telemetry preview under SoftAP. Changing MQTT config uses stop→NVS→start with the same SoftAP pause path; `mqtt stop busy` refuses to persist. Review focus: SoftAP+MQTTS headroom, avoiding IDF phy abort under NO_MEM, and smaller MQTT outbox during SoftAP. **Update 2026-11-24 (measured on hardware):** the single biggest slack was the render task's 32 KB stack — its deepest path needs 3.6 KB. At 16 KB the steady state went from 5 KB / 3.3 KB (int free / largest) to 21.8 KB / 13.3 KB; before that, the first stop of MQTT (every URL display, OTA, broker change) left it unable to restart, and an HTTPS image starved lwIP and httpd. Measure task stacks with `GET /api/v1/system/getDiag` (`stackMin`) before adding gates.

---

## 11. 2026-11-24 真机实测与修正（1.3.10）

测试条件：一台 ESP32-C61HR2，STA + httpd + MQTT over TCP，串口台架，局域网。以下读数来自固件自己打的日志和 `GET /api/v1/system/getDiag`。

### 11.1 修正前（1.3.9 与 1.3.10 首版在这些路径上相同）

| 读数 | 数值 |
|------|------|
| `esp_wifi_init` 前 int free / largest | 48.5 KB / 27.6 KB |
| MQTT 首次启动前 int free / largest | 12.5 KB / **8.7 KB** |
| 稳态（MQTT 已连）int free / largest | **5.4 KB / 3.3 KB** |
| MQTT 第一次停止后 largest | **4.1 KB**，此后一直 `deferred_heap` |
| HTTPS 取图期间 int free | **1.4 KB**（largest 736 B），TLS 读取失败 |

后果（都在真机上复现）：

- 第一次 `displayUrl`（HTTP 或 HTTPS、MQTT `display_url`、CYCLE `wakeUrl` 走同一路径）之后 MQTT 再也起不来，直到重启。
- HTTPS 取图把内部 RAM 耗尽后，网页、MQTT、局域网 ping 全部无响应。
- URL 升级建不出 12 KB 的专用任务：`task spawn failed`。
- `setMqttParam` 改配置走的是同一个 stop → start，同样受影响。

### 11.2 根因

`epd_render` 任务的栈是 **32 KB**（内部 RAM，FreeRTOS 栈不能放 PSRAM），而真机上把所有会在该任务上运行的路径跑了一遍——HTTPS 握手、JPEG/PNG 解码、抖动、画布 + 二维码、长文本折行、原图上传、代理取图、两个解码错误路径——**最深的只用了 3568 字节**。32 KB 里有约 28 KB 从头到尾闲着，比整机稳态剩下的内部 RAM 还多。

### 11.3 修正

| 改动 | 位置 |
|------|------|
| 刷屏任务栈 32 KB → **16 KB**（最坏路径的 4.5 倍） | `app_epd.c` `RENDER_TASK_STACK` |
| cJSON 节点与键串优先 PSRAM | `main.c` `json_mem_init()` |
| URL OTA 在刷屏任务上下载，不再新建 12 KB 任务 | `ota.c` `ota_url_run()`、`http.c` `setDevUpgrade` |
| MQTT 任务栈 3072 → **4096**，状态/遥测/应答发布不再在栈上放 `devInfo_t` + `mqttAttr_t` | `mqtt.c`、`config.c` |
| 诊断字段移出 `getDevInfo`，单独放 `getDiag` | `http.c` |
| 系统事件任务栈 2304 → **3072** | `sdkconfig.defaults` |
| URL 图片的整块缓冲在传输开始前预留，读到 `Content-Length` 即收缩到实际大小 | `app_epd.c` `epd_url_frame()` |
| 渲染任务消息改用启动时预留的 3 个槽位；槽位下方留一个 20 KB 空洞收留小对象 | `app_epd.c` `RENDER_MSG_SLOTS`、`SMALL_OBJ_NURSERY` |
| URL 刷图 / 代理取图跟随最多 5 次跳转 | `app_epd.c` `url_open_follow()` |

### 11.4 修正后

| 读数 | 修正前 | 修正后 |
|------|--------|--------|
| `esp_wifi_init` 前 int free / largest | 48.5 KB / 27.6 KB | **64.1 KB / 34.8 KB** |
| MQTT 首次启动前 int free / largest | 12.5 KB / 8.7 KB | **19.3 KB / 18.4 KB** |
| 稳态 int free / largest | 5.4 KB / 3.3 KB | **20.4–21.9 KB / 12.8 KB** |
| HTTPS 取图期间 int free（最低点） | 1.4 KB | **17.4 KB** |
| 刷图 / 升级失败之后 | MQTT 不恢复 | **每次都立即恢复** |
| MQTT 连接成功后的栈余量 | 168 B | **1732 B** |
| `sys_evt` 栈余量 | 640 B | **1408 B** |

最终构建上的一轮 18 步压力（HTTPS JPEG、HTTPS PNG、HTTP JPEG、857 KB JPEG、画布 + 2 个二维码、长文本、原图上传、测试图、HTTPS 代理取图、2400×1800 PNG、截断 PNG、65535×65535 JPEG、再一次 HTTPS JPEG、857 KB JPEG、2400×1800 JPEG、URL 取 960000 字节原生帧、1.4 MB 超限文件、带跳转的 HTTPS 图片）：**零重启**，18 步之后 MQTT 都恢复，内部 RAM 在各步之间保持在 20.4–21.9 KB，没有下滑趋势。11 步成功；截断 PNG、超大尺寸 JPEG、超限文件按预期被拒绝。早期「刷图后大图 / 原生帧被拒」的条目在分段读入落地后已消除，见下。

2026-11-24 晚间同一台架（`192.168.31.236`，镜像 `dist/ek410.bin` sha256 前缀 `5fc910f9736a`）复测：

- 13 步刷屏 soak（小 PNG、HTTPS PNG、~0.9 MB 基线 JPEG×2、原生帧夹在 JPEG/PNG 之后×3、302 跳转 PNG/原生帧、渐进式 JPEG / 超限 / 截断按预期失败、大 PNG）：**13/13**，每步后 MQTT 恢复，`freeInt` ≈ 21 KB。
- `tools/fix_verification.py`（含 `--url` / `--raw-url` / `--firmware` / `--power`）：**10/10**。
- 真实 HTTPS URL 升级失败（不存在的 GitHub release 包）：数秒内 `otaState=failed`（`ESP_FAIL`），MQTT 立即恢复，设备未重启。

各任务最小剩余栈（字节，fix_verification 末次）：`epd_render` 15960（16384）、`mqtt_task` 1728（4096）、`httpd` 2072（5120）、`sys_evt` 640（3072）、`esp_timer` 1312（3584）、`button` 1236（3072）、`led` 1240（2048）。`sys_evt` 在 GOT_IP 链路上仍偏紧，但高于崩溃线。

### 11.5 未解决 / 未验证

- **成功刷过一张图之后，PSRAM 最大连续块仍可能变小**（现象仍在，业务影响已缓解）。总空闲约 1.1 MB 不变，最大块常落在 ~0.5–0.8 MB。本版 URL 正文最多分 3 段读入，JPEG/PNG 经回调顺序读分段缓冲，原生帧同路径——因此不再因「需要一整块 937 KB / ~857 KB」而拒绝。谁占着中间那块仍未用诊断构建钉死。
- **渐进式 JPEG 无法解码**（ROM TJpgDec 只支持基线）：报 `JPEG decode fail`。属格式限制，非内存问题。
- **MQTTS（TLS MQTT）没有在真机上测**：台架 broker 为 TCP。
- **URL 升级成功路径未测**：本轮只补测了真实 HTTPS 的**失败**路径；台架没有可下载的正式 HTTPS 固件包。
- NimBLE 协议栈常驻（为了随时可进 BLE 配网）占的内部 RAM 没有动，这是最大的一块可再腾出的预算，但改动面大。
- 以上都是**一台设备**的数据；多台设备和 72 小时长测还没做。

---
