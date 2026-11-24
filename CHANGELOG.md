# Changelog

All notable changes to the CamThink **EK410** firmware are documented here.

Format follows [Keep a Changelog](https://keepachangelog.com/). Versioning follows [Semantic Versioning](https://semver.org/).

中文说明见各版本条目下的 **中文** 小节。

---

## [Unreleased]

## [1.3.10] — 2026-11-24

Release-readiness audit of 1.3.9. Every change below is from static review plus
host checks (clean build, host unit tests); **none of it has been validated on
hardware yet** — run `tools/qa_gate.py` candidate + 72 h soak before shipping.

### Fixed

- **URL image display corrupted the PSRAM heap on every JPEG/PNG** (HTTP `displayUrl`, MQTT `display_url`, CYCLE `wakeUrl`): `epd_url_frame()` freed the dithered ink buffer before streaming it and freed it again. Regression since 1.3.4.
- **Deeply nested JSON rebooted the device** on every HTTP route, CT-Prov (HTTP and BLE) and MQTT downlink (cJSON recursion vs. 4–5 KB task stacks). Input is depth-bounded before it is parsed.
- **`setDevInfo` with a `devName` of 4000+ bytes aborted** in NVS. Names are limited to 32 bytes and rejected with an error.
- **JPEG size arithmetic wrapped in 32 bits**: a crafted SOF could allocate a small buffer and overflow it. Sizes are 64-bit with dimension caps. Large JPEGs (> ~7.9 MP, forced SPEED) now display by integer subsampling after the 1/8 decode instead of failing.
- **PNG decode errors leaked the RGB and row buffers** (up to 720 KB PSRAM per failed URL).
- **CYCLE could stay awake for the rest of a boot** — battery drain: after a short button press, after a BLE/SoftAP window closed without STA, or when the boot never got an IP. The window is re-armed on key release and on the next `GOT_IP`; an offline boot falls back to the max window.
- `sleep_go` re-checks provisioning after the panel wait: SoftAP opened during a long refresh was cut by the sleep.
- Hibernate saved under SoftAP is entered when the hotspot closes (the UI already said so).
- MQTT `sleep` / `set_power` follow-up error acks carry the requesting command; timer-driven entries no longer ack.
- **MQTT was recreated 3 s into an OTA or URL image download** that had just stopped it for heap. Both hold the restart until they release.
- **URL OTA failure was invisible**: `getDevInfo` reports `otaState` / `otaError`; the web UI polls them instead of showing "success".
- Web UI: the device-name field is no longer reverted by the status poll while typing; the image-quality toggle no longer saves an unsaved work mode (it could hibernate the unit); System → Save does not post power defaults when the power form failed to load; a unit in `always` mode with a leftover `sleepMin` is shown as `always` (it used to be shown as cycle, and System → Save then switched it to timed sleep); editor file names are HTML-escaped; undecodable image uploads and blocked `localStorage` report instead of failing silently; overview uptime is live.
- Factory gesture needs both keys held for the whole 6 s. `wifi_ble_prov_enter()` is re-entrancy safe. A failed SoftAP open restores the CYCLE window; SoftAP close always arms the reconnect backoff.
- `sleepMin` is clamped (≤ 1 year) and a timer-wake setup failure no longer aborts into a reboot loop.
- Power / devName / factory-reset NVS writes return errors instead of `ESP_ERROR_CHECK` aborts. `clearEventLog` and factory reset report real results.
- SoftAP / BLE provisioning windows close after 15 min even while a client is attached.
- `proxyImage` jobs dropped from the queue answer with an error instead of HTTP 200 + empty body; waiter/runner hand-off is race-free.
- MQTT error events are logged once per changed failure (5 min) — a dead broker used to erase event-log sector 0 ~2000×/day.
- `lastfrm` skips the 1 MB erase when the frame is unchanged.
- Factory reset also wipes the NimBLE bond store.
- Panel refresh BUSY timeout 40 s → 60 s; AW9523B I2C uses a 100 ms timeout; canvas text ignores `\r` and other control characters; CT-Prov `config.set` rejects unknown sections before applying anything; a busy wake-URL queue no longer runs HTTPS on the 2.3 KB event task.
- MQTT no longer retries every 5 s when no broker is configured. A saved broker config clears the previous attempt's error, so "Test connection" cannot report the old failure for the new config. `ota_upload_abort()` keeps retrying for its mutex instead of leaving OTA busy until reboot. CT-Prov's MQTT save adds no stack on the 4 KB BLE worker.
- Web UI: the "hotspot lost" banner needs three missed polls (the MQTT bring-up pause raised a false alarm); overview placeholders show "—" instead of mock values (including a hard-coded MAC).

### Changed

- **`wakeSec` default is 120 s again** (it had silently become 600 between 1.3.0 and 1.3.2); README, API docs and the web UI now agree.
- **`getMqttParam` no longer returns the MQTT password** (`hasPassword` instead, like the Wi-Fi password). A blank password keeps the stored one only for the same broker and username; changing either clears it and `setMqttParam` answers with a `warn`. QA tools take `--mqtt-password` for the cases that change the broker or publish downlink.
- `setPowerParam` with neither `mode` nor `sleepMin` leaves the sleep schedule untouched.
- Rollback gate: a pending-verify image is confirmed only once the panel probe passed and the unit is reachable (web up or STA online).
- Release build is explicitly `-Og` (see `sdkconfig.defaults`); the Wi-Fi buffer options were removed from `sdkconfig.defaults` — the runtime table in `wifi.c` is authoritative.
- Font bitmaps come from **Noto Sans CJK SC** (16×16 CJK) and **Noto Sans Mono CJK SC** (8×16 ASCII), both SIL OFL 1.1, instead of macOS system fonts; `qrcodegen.min.js` carries its MIT notice again.

### Added

- `docs/security.md` / `docs/zh/security.md`: what the unauthenticated LAN/SoftAP API exposes, and the decisions to take before mass production.
- `getDevInfo` diagnostics: `heap` (`free`, `minFree`, `freeInt`, `minFreeInt`, `largestInt`, `freeSpiram`, `largestSpiram`) and `stackMin` (smallest free stack ever seen, in bytes, per long-lived task) — the numbers to watch when validating a build on hardware.
- Host regression tests: `python3 tools/run_host_tests.py` (`tools/tests/`: JSON depth guard, native frame layout, QA gate semantics, embedded page in sync / deterministic / i18n consistent, font tables, repo hygiene). `tools/build_release.sh` runs them before building.
- `tools/fix_verification.py`: checks every fix of this release on a real device over HTTP — JSON depth, `devName` limit, MQTT password hidden, failed URL OTA reported, URL display soak, local-upload OTA round trip, `sleepMin` clamp — and fails on 1.3.9 firmware. Part of `qa_gate.py --tier candidate` (`--verify-url`, `--verify-rounds`).
- QA gate: `firmware_binding` case (device `fwVersion` must equal the image's version), an all-skipped run is not `ok`, `--mqtt-password` pass-through.
- `tools/frame_convert.py` now writes the real native layout (CS0/CS1 halves, 90° rotated) — its old output was scrambled on the panel.

### 中文

- **URL 刷图堆损坏**（HTTP / MQTT / CYCLE 唤醒图）：码流缓冲被提前释放又二次释放，1.3.4 起的回归。
- **深层嵌套 JSON 导致重启**：HTTP、CT-Prov（含 BLE）、MQTT 下行全部受影响，解析前限制嵌套深度。
- **`devName` 超 4000 字节触发 abort**：限制 32 字节并返回错误。
- **JPEG 尺寸计算 32 位回绕可堆溢出**：改 64 位并限制尺寸；大照片改为 1/8 解码后整数抽样，不再直接失败。
- **PNG 解码失败泄漏** RGB 缓冲。
- **CYCLE 多条路径永不休眠**（短按按键、配网关闭时未联网、开机拿不到 IP）：按键松开和联网时补布防，离线开机用最大窗口兜底。
- OTA / URL 下载期间 MQTT 不再被 3 秒自动重试拉起；URL 升级失败网页如实提示并轮询 `otaState`。
- 网页：设备名不再被轮询冲掉；画质开关不再连带保存未确认的工作模式；常开设备不再因残留的 `sleepMin` 被显示成定时刷新（此前点保存会把设备切到定时休眠）；上传不可解码图片、浏览器禁用存储不再静默失败；运行时长显示真实值。
- 出厂手势需双键持续按住；BLE 进入防重入；`sleepMin` 加上限且定时器失败不再重启循环；NVS 写入失败不再 abort；配网窗口 15 分钟硬上限；出厂重置同时清除蓝牙配对。
- **`wakeSec` 默认恢复为 120 秒**（此前被悄悄改成 600）。
- **`getMqttParam` 不再回传 MQTT 密码**（改为 `hasPassword`）；更换 broker 或用户名时空密码不再沿用旧密码，QA 工具新增 `--mqtt-password`。
- 汉字点阵改用 **Noto Sans CJK SC**、ASCII 点阵改用 **Noto Sans Mono CJK SC**（均为 OFL 1.1）；恢复 qrcodegen MIT 声明。
- 新增安全说明文档；QA gate 绑定设备实际固件版本、全跳过不算通过；`frame_convert.py` 改为真实的原生帧布局。
- 未配置 MQTT broker 时不再每 5 秒空转重试；保存新配置后清除上一次的错误，测试连接不会误报旧错误。
- `getDevInfo` 新增 `heap`（堆余量）与 `stackMin`（各任务最小剩余栈）诊断字段。
- 新增主机回归测试 `python3 tools/run_host_tests.py`（发布脚本会自动跑），以及上机核对工具 `tools/fix_verification.py`（已接入 `qa_gate.py --tier candidate`，在 1.3.9 固件上会逐项报失败）。
- 以上均为静态审查 + 主机检查的结果，**尚未上机验证**。

## [1.3.9] — 2026-11-23

### Fixed

- Add `mqtt_restore_after_temp_stop()` and use it for URL TLS (incl. SoftAP+STA), OTA fail restore, HTTP/CT-Prov MQTT save-fail after stop.
- `mqtt_stop` takes the mutex before cancelling the start-retry timer (busy return no longer strands deferred_heap recovery).

### 中文

- 统一临时 stop 后恢复；SoftAP 下 URL/OTA 也会拉回 MQTT；stop busy 不再误掐重试定时器。

## [1.3.8] — 2026-11-23

### Fixed

- MQTT `deferred_heap` no longer sticks forever: `mqtt_stop` re-arms start-retry on STA; coalesced `mqtt_start` arms retry; URL TLS release kicks retry after failed restore.
- `mqttReason` reports `deferred_heap` only while largest internal block is actually too small (else `connecting`).

### 中文

- 修复 MQTT deferred_heap 粘连：stop 后重武装重试、URL 刷屏失败强制 kick、状态不再假粘连。

## [1.3.7] — 2026-11-23

### Fixed

- Render queue supersede now publishes MQTT completion `error` ack (no stranded `queued`).
- CYCLE `sleep_timer` spawn failure clears `s_entering` and re-arms (no sticky sleep gate).
- SUBSCRIBE-fail teardown uses timer retry if stop-task create fails.
- `mqtt_publish_telemetry` / `publish_up` return broker publish success; `ping` acks honestly.
- SoftAP hibernate save toasts deferred sleep instead of false "sleeping".
- `setMqttParam` restores MQTT if NVS save fails after a preparatory stop.
- Proxy image HTTP open failure calls `url_tls_release`.
- Deep-sleep abort (panel busy / provisioning) sends follow-up MQTT `error` ack.
- Last-frame flash write failure invalidates meta and aborts capture.

### 中文

- 队列丢弃补完成 ack；sleep 定时器 spawn 回滚；zombie stop 定时器重试；telemetry/ping 诚实；SoftAP hibernate toast；NVS 失败恢复 MQTT；proxy TLS 释放；深睡推迟补 ack；lastfrm 写失败清 meta。

## [1.3.6] — 2026-11-23

### Fixed

- MQTT SUBSCRIBE failure tears down the client (no zombie `mqtt_is_started` session).
- `rx_reset` no longer frees `rxBuf` without the client mutex (DISCONNECT vs stop race).
- MQTT/HTTP `sleep_now` / hibernate report error when spawn fails (no false success ack).
- Deep-sleep waits longer for `mqtt_stop` before forcing Wi‑Fi off.
- `render_text` OOM / fill failure clears `lastDisplayOk` so completion `cmd_ack` is honest.
- SoftAP MQTT save reply-loss path no longer POSTs `testMqtt` (poll `getDevInfo` only).
- MQTT uplink publish retries mutex take and logs `evtlog` on final drop (no silent ack loss).

### 中文

- SUBSCRIBE 失败销毁会话；rx_reset 不再无锁释放；休眠/休眠 ack 诚实；深睡延长 mqtt_stop 等待；文本 OOM 刷新 lastDisplayOk；SoftAP 边缘禁止 testMqtt；publish 互斥重试并记日志。

## [1.3.5] — 2026-11-23

### Fixed

- MQTT fragment reassembly holds the client mutex so `mqtt_stop` cannot free `rxBuf` mid-copy.
- Panel stream/URL paths honor `epd_commit_display` failure (no false lastfrm / displayed).
- SoftAP open restores MQTT if Wi-Fi bringup fails after `mqtt_stop`.
- SoftAP/BLE close re-enters `sleep_on_sta_online` so CYCLE `wakeUrl` is not skipped.
- MQTT `clear` / `render_text` / `render_canvas` publish completion `cmd_ack` after the panel job.
- SPA editor no longer double-toasts success; Markdown push waits for `displayBusy` like the editor.
- `testMqtt` / SoftAP keep_live / session paths require STA for `connected`.

### Changed

- `CMakeLists.txt` sets `PROJECT_VER` from `VERSION` so `esp_app_desc` matches `fwVersion`.

### 中文

- 分片重组与 stop 互斥；刷屏 commit 失败如实上报；SoftAP 半开失败恢复 MQTT；配网关闭补跑 wakeUrl；MQTT 刷屏完成 ack；SPA 推屏 toast/等待修正；connected 统一要求 STA；镜像版本绑定 VERSION。

## [1.3.4] — 2026-11-23

### Fixed

- CYCLE sleep: button-wake provisioning no longer sets `s_cycleStarted` early, so GOT_IP after SoftAP/BLE close can arm the wake window again.
- MQTT uplink publish (`cmd_ack` / status) holds the client mutex so `mqtt_stop` cannot destroy the client mid-send.
- SoftAP httpd pause incomplete drops the sticky pause claim (resume) so SoftAP/LAN web can self-heal; if httpd is still up the claim is cleared immediately.
- BOOT (IO9) factory reset now `esp_restart()` like button/HTTP factory.
- URL Quality dither failure frees the RGB buffer (was leaking hundreds of KB PSRAM).
- `uploadFrame` finish returns error on size mismatch / commit failure (HTTP reports failure).
- `sleep_now` / hibernate refuse SoftAP with an explicit error instead of HTTP/MQTT success + silent no-op.
- Last-frame flash: write body before header; stream verifies CRC before painting the panel.
- `getMqttParam.connected` requires STA (aligned with `getDevInfo.mqttConnected`).

### Changed

- `tools/build_release.sh` runs `bump_version --check` + `gen_www.py` before build.
- `gen_www.py` uses gzip `mtime=0` for deterministic `www_html.h`.
- README SoftAP SSID docs use `EK410_<MAC6>` (matches firmware).

### 中文

- 修复按键唤醒后 CYCLE 睡眠可能永不布防、MQTT 发布与 stop 竞态、httpd pause 粘连、IO9 出厂不重启、Quality dither 泄漏、上传误报成功、SoftAP 下休眠假成功、lastfrm CRC/写序、MQTT connected 语义。
- 发布脚本增加版本/网页预检；gzip 确定性；README SoftAP SSID 文案对齐。

## [1.3.3] — 2026-11-23

### Changed

- LAN MQTT “Test connection” now saves then polls `getDevInfo` (same path as SoftAP); no longer blocks on sync `testMqtt`.

### Fixed

- MQTT display commands (`clear` / `render_text` / `render_canvas` / `display_url`) publish `cmd_ack` status `queued` on accept (was `ok` + detail `queued`), matching HTTP and CHANGELOG async semantics.
- Fragmented MQTT downlink chunk gap/overflow/orphan mid-chunk now returns `cmd_ack` error `fragment desync` instead of silent drop.

### 中文

- LAN「测试 MQTT」与 SoftAP 一致：保存后轮询状态，不再同步卡在 testMqtt。
- 刷屏类下行即时 ack 状态改为 `queued`；分片失步/溢出时上报 `fragment desync`。

## [1.3.2] — 2026-11-20

### Added

- Web local firmware upload (`POST /api/v1/system/uploadFirmware`) alongside HTTPS URL OTA; pending-verify reboot after success.
- Markdown mode: Spectra-6 background and text color controls (preview + push).

### Changed

- System page desktop layout: full-width 2-column cards (work mode / OTA stay wide).
- MQTT QoS option labels use standard QoS 0/1/2 wording; topic details copy cleaned up.

### Fixed

- STA/LAN MQTT start no longer pauses httpd (pause was leaving the device IP unreachable).
- Local OTA upload failure restores MQTT and re-arms cycle sleep; busy MQTT stop refuses the upload.
- HTTPS URL OTA now stops MQTT before download (same as local upload), refuses when upload is busy, and restores MQTT + sleep on failure / task spawn failure.
- SoftAP “Test connection” only reuses a live MQTT session when the form is unchanged since last load/save; dirty saves without an HTTP ack report unknown instead of trusting the old session; baseline is locked to the submitted payload so edits during the request stay dirty; post-save `loadCfg` keeps local MQTT edits; oversize CA errors re-enable Save/Test buttons.
- Cold-boot STA join: recoverable disconnects during `wifi_sta_start()` (ASSOC_FAIL 203 / AUTH_FAIL / handshake / no-AP) trigger up to 3 quick in-flight reconnects instead of waiting the full 20s timeout; settle delay after disconnect raised to 250ms.

### 中文

- 网页支持本地固件上传；Markdown 可调背景/文字六色。
- System 桌面端双列布局；QoS 文案规范化。
- 修复 STA 起 MQTT 误停局域网网页、OTA 失败后未恢复 MQTT/休眠、SoftAP 测试误报旧会话成功（含保存无应答与请求中再编辑）；保存后回填不覆盖本地新编辑；超长 CA 不再卡死按钮。
- HTTPS URL OTA 与本地上传对齐：先停 MQTT、busy 互斥、失败恢复 MQTT/休眠。
- 冷启动首次关联失败（如 reason=203）在连接等待窗口内快速重试，不再傻等 20 秒。

## [1.3.1] — 2026-11-19

### Changed

- Single firmware image: SoftAP XOR BLE CT-Prov in-process (no dual OTA SKU / reboot switch).
- NimBLE enabled in `sdkconfig.defaults`; PAGE/MAIN toggle NimBLE without changing boot slot.
- Release entry: `tools/build_release.sh` / `flash_release.sh` → one `dist/ek410.bin` (legacy `*_dual_sku.sh` are aliases). SoftAP↔BLE smoke: `ct_prov_xor_test.py`.
- Keep SoftAP-oriented Wi-Fi buffer defaults; do not merge old BLE-SKU tighter RX sizes into the single image.
- MQTT fragmented commands transfer the reassembly buffer to the cmd queue (no second full copy); OOM / size / queue-full return explicit acks.
- Event log: no full rewrite on download; HTTP stream snapshots then sends outside the mutex; header persist every 8 lines (sector 0 only); boot recovers incomplete tails; compact only when over capacity.
- Async display HTTP/MQTT accept replies use `queued` / `queue full` / `sleep pending`; SoftAP shows event-log unavailable on old partition tables.
- `mqtt_stop()` returns success/busy; BLE/SoftAP enter use `mqtt_stop_wait()` and abort+restore on timeout (no NimBLE start over a live MQTT client).
- Stop telemetry timer only after taking the MQTT mutex; `mqtt_restart()` / CT-Prov disable propagate stop failure so a busy client keeps periodic telemetry.
- BLE exit: coalesce heap, lean `wifi_init` after NimBLE, httpd before wifi_init + retry timer; `wifi_sta_reconnect_saved(0)` starts Wi‑Fi after teardown; `mqtt_stop` cancels start-retry so MQTT cannot restart mid-BLE.
- MQTT broker change (HTTP `setMqttParam` + CT-Prov `config.set`): stop old client first, then persist host/CA, then start — busy stop keeps old NVS and live connection.
- `cfg_set_mqtt_ca_pem` / `cfg_set_mqtt_attr` return NVS errors; CA write rolls back RAM on failure; Web reports `mqtt CA save failed` / `mqtt config save failed` instead of false OK.
- QA gate: `tools/qa_gate.py` + `qa_lib` (smoke/candidate/soak, JSON statuses); plan in `docs/qa_automation.md`.
- LAN Web: embed gzip `INDEX_HTML` (~63KB wire), chunked send, `max_open_sockets=3`; cold boot uses lean Wi‑Fi buffers; MQTT requires `largest_int≥10KB`; defer httpd until STA online so opening the device IP no longer triggers `wifi:mem fail` / `phy_track_pll` NO_MEM abort.
- Product loops: `mqttReason` / `provError` / `displayBusy` / `sleepDeferred` / batterySense; SoftAP+BLE Web entry; port↔transport warn; OTA confirm on GOT_IP/httpd; sleep/MQTT-busy/display completion surfaced in SPA.
- SoftAP+MQTT: pause SoftAP httpd → start MQTT → resume lean httpd (2 sockets); skip telemetry preview while SoftAP up so customers can join home Wi‑Fi and see `mqttConnected` before closing the hotspot.
- SoftAP MQTT test UX: reply `setMqttParam` before pause; UI polls `getDevInfo` (no racing `testMqtt`); pause API returns real stop status; `mqtt_open` checks task create; QoS change forces reconnect; expose `mqttLastError` / `mqttInfraOk`.

### 中文

- 单镜像：SoftAP 与 BLE CT-Prov 进程内互斥启停，不再切换 OTA SKU。
- 发布入口改为 `build_release.sh` / `flash_release.sh`，只产出一份 `ek410.bin`；旧双 SKU 脚本名保留为别名。
- MQTT 分片所有权转交队列；事件日志下载不再全分区擦写；旧分区表上 Web 明确显示日志不可用。
- 更换 MQTT broker：先停旧客户端再写 NVS；停止忙时保留旧配置与旧连接。
- MQTT CA / broker 持久化失败会返回错误；CA 写失败时回滚内存中的 PEM。
- 自动化验收入口 `qa_gate.py`（smoke / candidate / soak）与方案文档。
- 局域网打开设备 IP 不再因首页过大 / 内部堆过紧而重启：gzip 首页、lean Wi‑Fi、MQTT 堆门槛、STA 未上线时推迟 httpd。
- 产品闭环：MQTT/配网失败原因、刷屏忙闲、睡眠推迟、端口协议告警、SPA 开 BLE。
- SoftAP 下可连 MQTT：短停网页腾堆 → 起 MQTT → 瘦身 httpd 恢复；遥测在 SoftAP 期跳过预览图。
- SoftAP「测试 MQTT」不再与停 httpd 竞态：先回保存响应再启动；前端轮询状态；QoS 变更会重连。

---

## [1.3.0] — 2026-11-17

### Added

- Dual OTA SKU: SoftAP (`ek410`) + BLE CT-Prov (`ek410_ble`); `tools/build_dual_sku.sh` / `flash_dual_sku.sh` / `ct_prov_dual_test.py`.
- NeoLink CT-Prov SoftAP + BLE provisioning (`docs/ct_prov.md`).
- Cycle power mode: configurable online window `wakeSec` (30–600, default 120) via SoftAP / HTTP / MQTT; DeviceTypes `ek410_Eink` updated.
- Hardware RESET / power-on: exit SoftAP/BLE handoff and restore flash `lastfrm` (CamThink logo if empty).
- Deep sleep: turn off status LED, panel rail, and battery sense before `esp_deep_sleep_start`.

### Changed

- SoftAP / BLE idle auto-close default **2 minutes**.
- BLE SKU warms Wi-Fi STA for scan after NimBLE (reduced RX buffers / BA win).

### Fixed

- SoftAP web: desktop first paint no longer flashes the mobile dock layout (FOUC).
- Wi-Fi `esp_wifi_init` `NO_MEM` after NimBLE blocking NeoLink scan.

### 中文

- SoftAP / BLE 双 OTA 镜像与 NeoLink CT-Prov；cycle 在线窗 `wakeSec` 可配。
- 硬件 RESET 退出配网并恢复上次画面；深睡前关灯与面板供电。
- SoftAP 网页桌面端首屏不再闪手机布局；修复 BLE 后 Wi‑Fi 扫描初始化失败。

---

## [1.2.2] — 2026-11-16

### Added

- Persist last successful panel frame in flash (`lastfrm` partition, 960000B native 4bpp).
- HTTP `GET /api/v1/display/getLastFrameMeta` and `GET /api/v1/display/lastFrame`.
- SoftAP: auto-load current panel into content preview; "Load from panel" button.

### 中文

- 成功刷屏后把当前画面写入 Flash；Web/API 可读取并预览。

---

## [1.2.1] — 2026-11-16

### Added

- SoftAP `POST /api/v1/display/proxyImage` — device STA fetches a remote image so the Web UI can preview same-origin (no browser CORS / SoftAP WAN dependency).
- Image mode: preview-first gate — push to panel only after URL/local preview is ready; push uses browser Spectra-6 dither + `uploadFrame`.
- Repo formalization: bilingual README, `docs/`, `VERSION` / `main/version.h`, `tools/bump_version.py`.

### Changed

- Centralized firmware version string (`EK410_VERSION_STRING`) via `main/version.h`.
- Asset layout: `assets/brand/`, `assets/splash/`; UI design preview under `docs/design/`.

### 中文

- 新增设备代拉图片预览接口；网页先预览再推屏。
- 版本号统一由根目录 `VERSION` + `main/version.h` 管理。
- 仓库目录与双语 README 整理为正式工程结构。

---

## [1.2.0] — 2026-11-13

### Added

- Image quality mode (Quality / Speed) persisted in NVS; SoftAP + power API.
- URL image stage progress text on panel (Quality path).
- SoftAP image URL / local upload / Markdown / canvas editor paths.

### Fixed

- SoftAP sheet / editor layout on phone; MQTT auto-connect after STA.
- English on-panel error strings for URL decode failures.

### 中文

- 图片质量档位、URL 刷屏阶段提示、SoftAP 显示与配网体验完善。

---

## [1.1.0] — 2026-11-12

### Added

- SoftAP config SPA (`main/www`), NeoMind MQTT topics, OTA dual-partition rollback.
- Panel bring-up on VE071A / EK410 pin map (`pindefine.h`).

### 中文

- SoftAP 配置页、NeoMind MQTT、OTA 回滚；面板引脚定稿。

---

## [1.0.0] — 2026-11-11

### Added

- Initial ESP32-C61 firmware skeleton: Wi-Fi, config NVS, EPD709 driver path.

### 中文

- 首个可构建固件骨架。
