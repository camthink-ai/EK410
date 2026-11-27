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

2026-11-24 晚间在同一台架（`192.168.31.236`）上，用 `tools/build_release.sh` 从 `main` 的源码和 `sdkconfig.defaults` 重新构建的镜像复测。

最终镜像 `2c5ba1cc5801`（在 `f541da6db01e` 之上加了 JPEG / PNG 的 700 KB 上限，其余相同）：

- `tools/fix_verification.py`（含 `--url` / `--raw-url` / `--firmware` / `--power`）：**10/10**，其中原生帧夹在图片之间连取 3 次全部显示。另：`setDevUpgrade` 给 `http://` 地址时在读任何字节之前即被拒绝（`url must be https://`）。
- 13 步刷屏 soak（HTTPS JPEG / PNG、HTTP JPEG、夹在图片之间的 960000 字节原生帧、857 KB JPEG、正好 700 KB 的 JPEG、700 KB + 1 字节与 760 KB 的 JPEG、2400×1800 JPEG / PNG、绝对地址跳转）：**10 步显示、3 步按预期拒绝（均为 `image >700KB limit`，启动后 1 秒内）、0 个意外失败、零重启**；每一步之后 MQTT 都恢复；内部 RAM 空闲在各步之间为 20.9–21.2 KB。
- 各任务最小剩余栈（字节）：`epd_render` 12968（16384）、`mqtt_task` 1736（4096）、`httpd` 2064（5120）、`sys_evt` 1408（3072）、`esp_timer` 1620、`button` 1236、`led` 1240。

此前的镜像 `f541da6db01e`（同一份源码，没有大小上限）：

- `tools/fix_verification.py`（含 `--url` / `--raw-url` / `--firmware` / `--power`）：**10/10**，其中原生帧夹在图片之间连取 3 次全部显示。
- 27 步刷屏 soak（HTTPS / HTTP 的 JPEG、PNG、原生帧；857 KB JPEG；2400×1800 JPEG / PNG；绝对地址、相对路径、两跳、https→https、http→https 跳转；跳转后的原生帧；渐进式 JPEG、跳转死循环、无 `Location` 的 307、截断 PNG、超大尺寸 JPEG、超限文件；画布 + 二维码、长文本、原图上传、测试图、HTTPS 代理取图）：**20 步显示、7 步按预期拒绝、0 个意外失败、零重启**；每个 URL 步骤之后 MQTT 都恢复；内部 RAM 空闲在各步之间为 20.5–21.9 KB。
- 真实 HTTPS URL 升级失败（下载到的是 JPEG）：3 秒内 `otaState=failed`（`ESP_ERR_INVALID_VERSION`），MQTT 立即恢复，未重启。该路径在刷屏任务上跑完 TLS，`epd_render` 栈仍剩 12656 字节。

各任务整轮最小剩余栈（字节）：`epd_render` 12656（16384）、`mqtt_task` 1728（4096）、`httpd` 2064（5120）、`sys_evt` 1408（3072）、`esp_timer` 1312（3584）、`button` 1236（3072）、`led` 1240（2048）。

> **构建配置的坑**：此前的镜像 `5fc910f9736a` 是在残留的 `build/` 目录里用 11 月 18 日的旧 sdkconfig 构建的，`CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE` 仍是 2304，`sys_evt` 实测余量只有 640 字节。`sdkconfig.defaults` 只对 sdkconfig 里还没有的选项生效，所以发布必须用 `tools/build_release.sh`（现已改为清理失败即停、删除残留的 sdkconfig、打印生效配置），不要直接 `idf.py build`。

### 11.5 未解决 / 未验证

- **网页轮询会切开空闲 PSRAM**（原因已用打印堆布局的诊断构建查清；业务影响已消除）。网页每隔几秒轮询一次 `getDevInfo`，HTTP 服务器为每条连接分配的 208 字节 TCP 控制块（lwIP 在这台机器上优先用 PSRAM）落在 MQTT 缓冲区上方，连接关闭后还要在 `TIME_WAIT` 停留 120 秒。MQTT 为下载一停，空闲区就被切成 826 KB + 230 KB 两块，总空闲约 1.1 MB 不变。本版 URL 正文最多分 3 段读入，JPEG/PNG 经回调顺序读分段缓冲，原生帧走同一条流式路径——不再要求一整块 937 KB / ~857 KB。
- **JPEG / PNG 越大，画面越软**（本版起解码结果经平滑放大后在整屏分辨率上抖动，代价是细节变少，不再是颗粒；见 11.6）。857 KB 的 JPEG 解码时 PSRAM 只剩约 2 KB（`getDiag.heap.minFree` 读到 2180）的问题，已用「超过 700 KB 直接拒绝」挡住；这里是上限之内仍然存在的一层。正文整块占着最大的空闲 PSRAM 块，解码器的输出缓冲只能从剩下的部分里取——预算规则 `rgb_budget_for()` 取「最大空闲块 − 48 KB（解码器工作池）」。网页轮询让空闲 PSRAM 分成约 800 KB + 230 KB 两块，于是 1600×1200 的 JPEG：
  - 66 KB：最大块 754 KB，预算 688 KB，解到 400×300（1/4，RGB888 需 360 KB）；
  - 624 KB：正文把大块吃到只剩 197 KB，最大块变成 230 KB，预算 176 KB，只解到 200×150（1/8）。总空闲其实还有 432 KB，只是不连续。

  按块大小推算，¼ 解码只维持到约 400 KB，400 KB 到 700 KB 上限之间都是 ⅛；2400×1800 的 JPEG 即使预算满额 720 KB 也不够 1/4（810 KB），始终是 300×225。可行的改进，按改动量从小到大：正文分段时优先填小块、给最大块留出 RGB 缓冲的空间，¼ 的分界点可提到约 640 KB；解码输出改用 RGB565（400×300 只要 240 KB），可一直覆盖到上限；按块边解码边抖动进面板缓冲，不再需要整块 RGB 缓冲，抖动分辨率还能从 400×300 提到 800×600。
- **渐进式 JPEG 无法解码**（ROM TJpgDec 只支持基线）：报 `JPEG decode fail`。属格式限制，非内存问题。
- **MQTTS（TLS MQTT）没有在真机上测**：台架 broker 为 TCP。
- **URL 升级成功路径未测**：本轮只补测了真实 HTTPS 的**失败**路径；台架没有可下载的正式 HTTPS 固件包。固件只接受 `https://` 地址并用公共 CA 包校验证书（`http://` 在读任何字节之前即被拒绝，已上机确认），所以成功路径需要一台设备信任的公网主机上的正式镜像；`tools/fix_verification.py --ota-url https://…` 已备好这项核对。
- **内部 RAM 在启动过程中一度只剩 8 字节**（本轮新测到，未定位）。全新开机 50 秒时读 `getDiag.heap.minFreeInt` 已是 8，说明最低点在启动阶段：Wi-Fi 初始化时内部空闲 64 KB（最大块 35 KB），MQTT 启动时 19.4 KB（最大块 18 KB），稳态 22 KB。启动一直成功，但那一刻任何内部 RAM 分配都会失败。定位办法是在各初始化步骤之后打印 `heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)`。也正因为水位开机就见底，长测里的 `minFreeInt` 无法再反映运行期的低点，只能看每轮的 `freeInt` / `largestInt`。
- NimBLE 协议栈常驻（为了随时可进 BLE 配网）占的内部 RAM 没有动，这是最大的一块可再腾出的预算，但改动面大。
- 以上都是**一台设备**的数据；多台设备还没做。72 小时长测（`tools/soak_long.py`，业务节奏：URL 刷图、原生帧上传、应被拒绝的文件、网页控制台、公网 HTTPS 图，全程串口监听）第一轮在 `2c5ba1cc5801` 上跑了 13.2 小时：383 轮，0 个意外结果、0 次重启，串口日志复核时发现「每次 URL 原生帧之后遥测没有预览图」（54 次全部如此，原因是提交上一帧时 960 KB 的正文还占着内存，已修），随后在最终镜像上重新开始。

### 11.6 URL 图片的抖动路径（2026-11-25 重写）

**旧路径**：解码 → 在解码分辨率上做误差扩散（1600×1200 的 JPEG 是 400×300，小图更低；每个像素 6 次浮点距离计算，约 50 µs）→ 最近邻覆盖采样放大到整屏（逐像素回调，约 20 秒）。屏上每个抖动点是 4×4 像素的方块，中间调是粗颗粒；灰字里混进红黄蓝（一张灰色标志图 11% 的像素是彩色的）。

**新路径**（`main/img_dither.c`，纯 C，主机测试直接编译同一份代码）：双线性放大在扫描时现算，不另开整屏 RGB 缓冲；在抖动网格上做蛇形误差扩散——`quality` 为 1600×1200（屏上每个像素一个墨点），`speed` 为 800×600（2×2 的点）；直接写进常驻的截帧缓冲 `s_cap`（960 KB，原生布局），随后原样流式送给面板。不再需要面板帧缓冲和墨色码缓冲。

- **工作内存**只在抖动期间存在：两张 4 KB 的墨色表 + 两列误差（`网格高 × 6 字节 × 2`）+ 每个网格行列的源坐标 4 字节，1600×1200 时约 35 KB，一次 `spiram_alloc`，用完即释放。不能常驻：取原生帧时 PSRAM 只剩约 90 KB 余量。
- **按列扫描**。面板的帧是列优先的（一行原生数据 = 一个逻辑列），所以沿逻辑列抖动、每列抖完顺序写出两个半区各 300 字节。第一版按行扫描、每个像素在 300 字节步长的地址上做一次读—改—写，每个像素一次 PSRAM 缓存未命中，一张图 8.8 秒。
- **编译级别**：只有这一个文件按 `-O2`（`main/CMakeLists.txt`），其余仍是 `-Og`。抖动 8.8 秒 → 5.7 秒。
- **近灰只用黑白**：判断依据是**源像素**的颜色（三通道相差小于 8），不是叠加误差之后的值；近灰像素的误差按中性白（248,248,248）记账。白墨的标称色是 (248,248,242)，每个白点让蓝通道的误差比红绿多 8，若按叠加后的值判断，纯灰区域三行之内就会漂进彩色表（实测 79% 彩色像素）。
- **墨色参考值走同一阶调曲线**。过去预处理只作用于图片、不作用于墨色，纯色区域每个像素都留下几级“误差”被一路往后带，几十个像素后数值饱和；饱和的色块把一股恒定误差推进相邻色块的第一列——蓝块旁边的绿块里冒出约 190 个蓝点。现在源像素恰好是墨色时误差为零。
- **踩过的坑**：帧里每个字节装相邻两个逻辑行（奇数行在高位）。按列扫描的第一版把高低位写反了，色块自检全部通过（色块内部上下两行同色），是和 `tools/frame_convert.py` 的逐字节对比抓出来的；自检因此加了一条逐行黑白交替的用例。

**真机上一次 URL 刷图的时间**（550×368 的 JPEG，`quality`）：下载 + 解码 1.3 秒 → 抖动 5.7 秒 → 传输 + 面板刷新 44.3 秒 → 保存上一帧 4.9 秒（擦除并重写 1 MB 分区 + 预览）。面板之外还剩约 12 秒，其中保存上一帧的 4.9 秒是原有逻辑。

**画布 / 文本**（`main/canvas.c`）：面板是一列一列接收的，所以每换一列为当前列建一张表（每个像素行被哪些图元覆盖，最多 4 个，超出则退回遍历整列的图元列表；每次渲染 `calloc` 6 KB，PSRAM）。文本图元的字位在渲染前一次性列好（每个图元约 2.4 KB），查像素时二分查找，并记住上一次找到的字位。此前每个像素都重新解码 UTF-8 字符串、并把所有图元的包围盒（在 PSRAM 里）测一遍：长文本一屏约 37–39 秒，现在约 2.6 秒（串口时间戳：任务入队到面板开始刷新）；输出帧与旧固件逐字节相同（192 万像素 0 差异，真机拉帧比对）。


### 11.7 蓝牙配网退出后的内部堆（2026-11-25，1.3.11）

**现象**（1.3.10，真机当天两次）：长按 PAGE 进入 BLE 配网，手机连上、配对、扫描 Wi-Fi，空闲 120 秒后设备自己退出。3 秒内重新拿到 IP、网页正常，但 MQTT 一直 `deferred_heap`（每 3–5 秒重试一次），直到断电重启——第二次从 16:42 一直持续到 22:50 被重启为止。

**根因**：进入配网时先把 Wi-Fi 驱动整个拆掉（给 NimBLE 腾出连续的内部内存），随后为了能扫描 Wi-Fi，又在 **NimBLE 占着内存的时候**重新初始化了一个精简配置的 Wi-Fi 驱动（`wifi_driver_ensure()`，lean）。NimBLE 停掉之后，它释放的内存被这个 Wi-Fi 驱动的分配隔成碎片——串口 `BLE CT-Prov stopped — int 30595 largest_int 7936`：内部空闲 30 KB，最大块不到 8 KB。接下来先起的 httpd 再占一块，MQTT 启动时最大块只剩 4096，而它需要 8192（`mqtt_internal_need()`）。没有手机连接时最大块正好是 8192——零余量；手机连过一次就掉到 7936。

**修正**（`wifi_ble_prov_close()`）：NimBLE 完全停止之后，把这个 Wi-Fi 驱动再拆掉一次（`wifi_driver_teardown()`，与进入配网时是同一个函数），让堆重新合并，然后按原有顺序起 httpd、起 Wi-Fi、入网、起 MQTT。

**修正前后**（同一台设备，用 `POST /api/v1/network/openBleProv {"idleSec":20}` 自动复现，没有手机连接）：

| 步骤 | 1.3.10 | 1.3.11 |
|---|---|---|
| NimBLE 停止后，最大块 | 8192 | 8192（碎片一样存在） |
| 重建 Wi-Fi 驱动后，最大块 | —（不重建） | 11264 |
| MQTT 启动时，最大块 | 8192（零余量） | 12800（与干净开机相同） |
| MQTT | 这次连上了 | 网页恢复后 2 秒连上，broker 侧遥测立即恢复 |

`tools/fix_verification.py --ble-exit` 把「退出后最大块 ≥ 10240」作为判定条件（1.3.10 的 8192 不通过）；`qa_gate.py --tier candidate` 会带上这一项。带手机连接的路径在这版镜像上**没有**自动复现的办法（这台电脑没有蓝牙主机，仓库的测试也只在进程内走 HTTP），待用 App 复测。

**曾经存在的余量问题（已修，见 11.12）**：本节上面的修正是 2026-11-25 的版本。之后在真机上看到，进过蓝牙配网的设备刷过一张 URL 图片后内部最大块只有 8704（干净开机是 12288–12800），比 MQTT 的下限 8192 只多 512 字节，整体内部空闲也少约 5 KB；当时推测是 NimBLE / 控制器未释放的部分，并给出了两条带取舍的出路（退出配网时释放控制器内存，或配网成功后重启）。第二天的排查找到真正的原因并修掉了：是配网的工作任务从不被删除，与 NimBLE 无关，两条出路都不需要了。

### 11.8 抓取失败之后的遥测预览（2026-11-26，1.3.11）

**现象**（真机，挂测新加的 broker 侧核对发现）：串口连续出现 `telemetry build frame_present=1 preview_b64=0`——HTTP 404 之后 2 条、`proxyImage` 之后 5 条、传输中断之后 2 条——直到下一次成功刷屏才恢复 `preview_b64=161440`。云端 / App 在这段时间看到的设备预览图是空的；面板显示本身不受影响。

**原因**：每次 URL 抓取之前，`url_tls_prepare()` 调 `last_frame_preview_drop()` 把 161 KB 的 base64 预览让给下载（注释原话：Preview b64 (~160KB) + HTTPS body fight for the same 2MB SPIRAM）。此后只有 `commit_frame()` 写入新帧时才会 `preview_rebuild_from_native()` 重建。抓取没有以保存新帧结束的三种情况——刷图失败、同一帧重复（`last frame unchanged — flash write skipped`）、只取图不刷屏的 `proxyImage`——预览就一直缺席。1.3.10 只修了「URL 原生帧之后」这一种（先释放正文再提交）。成功刷图期间不受影响：MQTT 是在帧保存之后才重启的。

**修正**：
- `last_frame_preview_restore()`：没有预览时 `preview_rebuild_from_flash()`，从 flash 读回已保存的帧重建。
- `app_epd_preview_restore()`：在**面板锁**下调用它。帧缓冲 `s_cap` 同时是渲染解码（`last_frame_buffer()`）和上传截帧的目标，不能和它们并发；最多等锁 2 秒，等不到（正在刷屏或上传）就跳过，下一次再说。
- 三个调用点：`render_task` 的 URL 失败分支（重放上一帧之前）；`proxyImage` 结束并释放正文之后（在 HTTP 处理函数里；等待方已超时的「孤儿」任务则在渲染任务里释放后重建）；`commit_frame()` 的「同一帧」分支（`s_cap` 里就是这一帧，直接重建，不用读 flash）。

**真机**：修复前的 12 轮里，串口 20 条遥测有 9 条是 `preview_b64=0`（404 之后、`proxyImage` 之后、传输中断之后，各持续到下一次成功刷屏）。修复后 12 轮——含 4 次抓取失败（404、传一半断开、卡住、自签名证书，都从 broker 下发）和 2 次 `proxyImage`——共 23 条遥测，全部带 161440 字节的预览。

**没有改的**：一次成功的 URL 刷图从抓取到保存新帧之间约 55 秒没有预览，这段时间 MQTT 是停的（帧保存之后才重启），所以遥测不受影响；只有失败和 `proxyImage` 才会把「没有预览」的状态带到 MQTT 在线的时候——这正是上面修掉的部分。

### 11.9 刷屏期间上传原生帧会让设备掉 Wi-Fi（2026-11-26，1.3.11）

**现象**（真机；挂测的第四种并发模式第一次跑到就发现，单独重复 4 次全部复现）：面板正在刷新时 POST `/api/v1/display/uploadFrame`，约 17 秒后设备报 `sta disconnected reason=200`，随后约 20 秒内扫描不到 AP（201），约 45 秒后才重新联网；上传必然失败，期间 MQTT 也断。不带上传的约 430 次刷屏里信标超时是 0 次。

**原因**：`display_upload_frame_handle()` 过去在 `epd_upload_start()` 里等面板锁最多 180 秒（74d37c7 引入，本意是让上传排在一次刷屏之后）。等待期间处理函数不读请求体，960 KB 的正文滞留在 TCP 连接里（接收窗口 5760 字节，很快关闭）。设备端日志 `bcn_timeout → ap_probe_send → disassoc` 表明确实收不到信标。**一段没人读的正文为什么会让射频侧失聪，没有查到机制**；5 次试验里因果是确定的。

**修正**：HTTP 上传不再等待：有刷屏在进行或排队（`app_epd_display_busy()`）、或面板锁被别人持有（`epd_upload_start_wait(0)`）时立刻回 `panel busy`；未读的正文由 ESP-IDF 的 HTTP 服务器在处理函数返回后读掉丢弃（`httpd_req_delete()`）。面板自己的重放路径（开机画面、恢复上一帧）仍用 `epd_upload_start()` 等 180 秒。

**真机**：修复后同样的请求 4 次都在 3 秒内收到 `{"code":1,"msg":"panel busy"}`，全程没有掉线，每张图刷完 MQTT 正常重连。

### 11.10 没有 Content-Length 的图片被解到 67×50（2026-11-26，1.3.11）

**现象**（真机，第四轮 72 小时长测跑到 8 小时时从串口日志里发现）：挂测的图片集里 `jpeg.jpg`、`chunked.jpg`、`nolength.jpg` 是同一张 1600×1200 的图。带 Content-Length 的 `jpeg.jpg`：`JPEG 1600x1200 → 400x300 (1/4)`；分块传输的 `chunked.jpg` 和 HTTP/1.0 无长度的 `nolength.jpg`：`rgb budget 48KB`、`JPEG 1600x1200 → 67x50 (1/8 subsampled)`（有时 100×75）。把设备存下的帧渲染出来对比：左（普通）标签框清晰，中（分块）标签框糊成一团、树和船变成色块。

**原因**：`epd_url_frame()` 在传输前预留一整块正文缓冲（最大 1280 KB）。已知长度时在响应头处就按 `Content-Length` 收缩（`heap_caps_realloc`，不移动块）；没有长度时 `cap = body_cap`，整块预留一直占着——传输期间如此是必要的，但下载完之后也没有归还：串口 `url mem[downloaded] free_s=71936 largest_s=67584`，解码预算规则 `rgb_budget_for()` 取「最大空闲块 − 48 KB」得 48 KB，于是 1/8 解码再整数抽样到 67×50。

**修正**：下载循环结束、知道实际长度之后，`body && clen <= 0 && total < cap` 时按 `total` 收缩预留，再记 `url mem[downloaded]`。

**真机**：修复后分块和无长度两种传输都是 `url mem[downloaded] free≈977KB largest_s≈950KB` → `rgb budget 720KB` → `JPEG 1600x1200 → 400x300 (1/4)`，留在设备上的帧 CRC 与普通传输相同（0x6fea78b7）。分步重放里又走了 6 次，全部如此。

**同一类里没改的**：带长度的大文件（620 KB 的 `photo.jpg` → 200×150，2400×1800 的 `large.jpg` → 300×225）仍是 11.5 节讲的「越大越软」——那次是正文本身占着大块，不是预留浪费。

### 11.11 一次未复现的上行故障（2026-11-26 上午，未结）

**现象**：某次开机（镜像 `a60f07c5bed3`）里，按 MAIN 长按开过热点、MAIN 单击关掉、PAGE 长按开蓝牙、MAIN 单击退出，之后连刷了几张 URL 图。开机第 757 秒（约 09:24:55）起，每条 162 KB 的遥测都发不出去：串口 `telemetry build` 之后**连续 39 秒一个字都不出**（连每 5 秒的电池电压都没有），然后 `transport_base: tcp_write error, errno=Connection reset by peer` → `MQTT: disconnected`，6 秒后重连、上线状态（94 字节）发得出去，30 秒后下一条遥测再次如此。期间 HTTP 接口不应答（`getDevInfo` 超时），ping 延迟 30–190 ms。持续到 09:47 重启为止，共 23 分钟。

**排除**：
- broker 和外网通路是好的：同一时间这台 Mac 往同一个 broker 发 160 KB（QoS 1）只要 0.3 秒，ping 36 ms。
- 不是环境：干净重启后同样的网络条件下，每条遥测 1.7–4 秒发完、broker 侧都收到。
- 局域网内另一台设备延迟相似（都是省电模式下的抖动），高延迟本身不说明问题；但出事期间设备下载 552 KB 用了 7.7 秒（72 KB/s，夜里是 210 KB/s）。

**分步重放**（干净重启后，每步之后量 3 条遥测的发送耗时）：4 张无长度 / 分块图片 → 开关热点 → 进出蓝牙 → 再一张无长度图片。全部 2–4 秒，每一步都正常，没有复现。

**不论触发条件是什么，两点本身就不对（本版没改）**：
1. 遥测是 `telemetry_timer_cb` 在 **esp_timer 任务**上调 `mqtt_publish_telemetry()` 发的。一条发不出去的 162 KB 遥测让这个调用卡住约 39 秒，esp_timer 任务上的所有定时器（电池采样、各种一次性延时）一起停——这就是串口全无输出的原因；HTTP 接口因为要拿同一把 MQTT 互斥锁（或被同一把锁挡住）也不应答。
2. 每 30 秒一条 162 KB（预览图 base64）对慢链路太重：MQTT 写超时 10 秒、broker 的保活时限 45 秒（keepalive 30 秒 × 1.5），链路吞吐一旦低于约 4 KB/s 就必然发不完。

**可选的改进（需要产品决定，涉及平台侧）**：预览图只在帧变化后的第一条遥测里带，其余心跳只发状态（上行量降到约 5%）；或把 keepalive 和写超时放宽；以及把遥测发布挪到独立任务上，卡住时不再拖住定时器。

**线索留档**：`~/ek410-uplink-episode-20261126/`——`verify_a60f.log`（出事前那次开机的串口，到故障开始前 85 秒为止）、`stall_watch.log` 与 `state_now*.log`（故障中的串口）、`evlog_now.txt`（设备事件日志）、`reboot_watch.log`（重启后恢复正常）、`repro_uplink3.out`（分步重放的结果）。挂测每轮核对遥测是否到达并全程保存串口，再出现会被抓住。

### 11.12 蓝牙配网的工作任务从不被删除（2026-11-26，1.3.11）

**现象**：进过一次蓝牙配网的设备，之后刷过一张 URL 图片，`getDiag.heap.largestInt` 是 8704（干净开机 12288–12800），`freeInt` 约 15–16 KB（干净开机约 20–21 KB）。MQTT 需要一块 ≥ 8192 的连续内部内存才能启动，所以它还能起来，但余量只有 512 字节。11.7 末尾当时推测是 NimBLE / 控制器未释放的部分，这一推测是错的。

**原因**：`ct_prov_ble_start_worker()` 在第一次进蓝牙时用 `xTaskCreate(worker_task, "ct_ble", 4096, …)` 创建工作任务；`worker_task()` 在 `xQueueReceive(s_job_q, &job, portMAX_DELAY)` 上无限等待；`ct_prov_ble_stop()` 把 NimBLE 主机和控制器全部拆掉，却从不删这个任务。于是它在这次开机的剩余时间里一直活着，4096 字节的内部栈（加任务控制块）留在第一次进蓝牙时分配的位置——正好在原本应是最大空闲块的中间，把它隔成两段。

**修正**（`main/ct_prov_ble.c`）：
- 工作任务每次最多等队列 500 ms；`s_want_up` 为假（配网已结束）且队列为空时自己退出：`s_worker = NULL; vTaskDelete(NULL);`。正在处理的一条命令会先做完（对端已断开时只是丢弃应答），队列清空后才退。
- `ct_prov_ble_stop()` 在拆完协议栈后等它退出，最多 3 秒；仍忙就先返回，任务做完手上的命令后自行退出。
- `ct_prov_ble_start_worker()` 用局部变量接收任务句柄；`s_worker` 是 `volatile`。晚到的一条 GATT 写入会照旧重建工作任务，它随后也会自行退出。

ESP-IDF 这一版的内核里，自删任务的栈由空闲任务回收，不看被删任务的优先级。

**真机**（镜像 `bef2d97c6f01`，用 `openBleProv {"idleSec":20}` 进出蓝牙，连做两次）：退出后内部最大块 14336，再刷一张图之后仍是 14336（修复前 8704）；第二次进蓝牙工作任务照常重建，配网可用。`tools/fix_verification.py --ble-exit --url …` 把「退出后再刷一张图，最大块 ≥ 10240」作为判定条件。

**没改的**：退出蓝牙后 Wi-Fi 仍是精简配置（`wifi_driver_teardown()` 置 `s_wifiLeanBuffers`，下一次 `esp_wifi_init` 用精简缓冲），这是既有设计，与余量无关。

---
