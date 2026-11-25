# SoftAP / 局域网 HTTP API

源码：[github.com/camthink-ai/EK410](https://github.com/camthink-ai/EK410)

基址：SoftAP `http://192.168.4.1`，或入网后的设备 STA IP。

多数接口直接返回 JSON 对象；错误形如 `{"code":1,"msg":"..."}`。

**接口没有鉴权**：以下所有接口对能访问到设备的任何终端开放（热点与局域网相同），见 [安全说明](security.md)。

限制：JSON 请求体 ≤ 16 KB、嵌套 ≤ 8 层（超限在解析前直接拒绝）；CT-Prov 请求体 ≤ 4 KB。

英文版：[api.md](../api.md)

产品侧 API 说明与示例：[CamThink Wiki · HTTP API](https://wiki.camthink.ai/zh-Hans/docs/epaper-ek410-series/software-guide/ek410-http-api) · [English](https://wiki.camthink.ai/docs/epaper-ek410-series/software-guide/ek410-http-api)

## 系统

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/api/v1/system/getDevInfo` | `devName`、`devMac`、`deviceId`、`fwVersion`、`ip`、`wifiConnected`、`wifiState`、`mqttConnected`、`mqttStarted`、`mqttReason`、`mqttLastError`、`httpServerUp`、`softapUp`、`bleProvUp`、`softApSsid`（热点开启时另有 `softApPass`）、`provisioned`、`panelOk`、`displayBusy`、`powerMode`、`sleepMin`、`wakeSec`、`uptime_s`、`batterySense` / `batteryMv` / `batteryPercent`、`otaPartition`、`otaPendingVerify`，以及 **`otaState`**（`idle` \| `running` \| `failed`）与失败后的 `otaError` |
| GET | `/api/v1/system/getDiag` | 按需读取的现场诊断（上机验证用）：`heap`（`free`、`minFree`、`freeInt`、`minFreeInt`、`largestInt`、`freeSpiram`、`largestSpiram`，单位字节）和 `stackMin`（各常驻任务历史上最小剩余栈，字节） |
| POST | `/api/v1/system/setDevInfo` | `{devName}`，UTF‑8 编码 1..32 字节；超长返回错误（`devName too long (max 32 bytes)`） |
| POST | `/api/v1/system/setDevReset` | 重启 |
| POST | `/api/v1/system/setDevFactory` | 清除配置、蓝牙配对与事件日志（保留 `deviceId`），然后重启 |
| POST | `/api/v1/system/setDevUpgrade` | `{url}`（≤ 511 字符），仅 HTTPS OTA。下载开始**之前**就返回 `{code:0}`：请轮询 `getDevInfo.otaState`，`failed` 时带 `otaError`，否则设备重启进入新固件。升级失败后设备自行恢复 MQTT 与休眠排程 |
| POST | `/api/v1/system/uploadFirmware` | 原始 `application/octet-stream` 应用镜像，200 KB – 4 MB，必须带 `Content-Length`；另有升级在进行时返回 `ota busy` |
| GET | `/api/v1/system/getEventLogMeta` | `{present,bytes,seq,capBytes,keepDays,timeOk}` |
| GET | `/api/v1/system/eventLog` | text/plain 下载 |
| POST | `/api/v1/system/clearEventLog` | 清空日志（失败返回 `event log clear failed`） |
| GET | `/api/v1/system/getPowerParam` | `mode`、`sleepMin`、`wakeSec`、`wakeUrl`、`imageQuality` |
| POST | `/api/v1/system/setPowerParam` | 字段同上；**请求体里出现哪些字段就只改哪些**。`mode` `always` \| `cycle` \| `hibernate`，`sleepMin` 1..525600（cycle），`wakeSec` 30–600（默认 120），`imageQuality` `quality` \| `speed`——URL 图片的抖动方式：屏上每个像素一个墨点（约 6 秒）\| 800×600、2×2 的点。返回 `sleepArmed`；休眠 / 周期暂时不能开始时带 `sleepDeferred: "softap"` 或 `"no_sta"`（热点关闭 / 联网后自动开始）。请求体既无 `mode` 也无 `sleepMin` 时不改变休眠排程 |
| POST | `/api/v1/system/sleepNow` | 立即深睡；SoftAP 开启时拒绝 |

## 网络

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/api/v1/network/getWifiParam` | `ssid`、`hasPassword`、`isConnected`、`state`、`ip`；密码永不返回 |
| POST | `/api/v1/network/setWifiParam` | `{ssid,password}`；同一 SSID 下密码留空表示沿用已存密码 |
| GET | `/api/v1/network/scanWifi` | `{aps:[{ssid,rssi,secure,authmode}]}` |
| GET | `/api/v1/network/getMqttParam` | `host`、`port`、`transport`（`mqtt` \| `mqtts`）、`tls`、`tlsVerify`（默认 true）、`caPem`、`clientId`、`qos`、`username`、`hasPassword`、`topicUp`、`topicDown`、`connected`；密码永不返回 |
| POST | `/api/v1/network/setMqttParam` | 字段同上。密码留空**仅在 host 与用户名都不变时**沿用已存密码；任一改变则清除旧密码，并在返回里带 `warn`。MQTTS：`tlsVerify`，可选 `caPem`（自定义 CA；留空用系统 CA 包）。SoftAP 下返回可能带 `mqttSoftApConnect` / `mqttSoftApSession` / `mqttDeferred`——网页改为轮询 `getDevInfo`，不再调用 `testMqtt` |
| POST | `/api/v1/network/testMqtt` | 连接探测（`connected` 要求 STA 在线）。SoftAP 下只做异步 / 已连接判断 |
| POST | `/api/v1/network/openSoftAp` / `closeSoftAp` | SoftAP 开关（切换完成前即返回；轮询 `getDevInfo.softapUp`） |
| POST | `/api/v1/network/openBleProv` / `closeBleProv` | BLE CT-Prov 开关；`openBleProv` 可带 `{idleSec}` 覆盖空闲超时（CI 用） |
| POST | `/api/v1/ctprov` | [NeoLinker](https://github.com/camthink-ai/NeoLinker) **CT-Prov/1** 绑定（JSON 信封 `{id,op,data}`；操作 `device.info.get`、`communication.capabilities.get`、`communication.scan`、`config.set`、`config.get`、`session.close`；详见 [ct_prov.md](../ct_prov.md)）。STA IP 上同样可达 |

## 显示

| 方法 | 路径 | 说明 |
|------|------|------|
| POST | `/api/v1/display/renderText` | `{text,color,bg,scale}` |
| POST | `/api/v1/display/renderCanvas` | `{bg,items[]}` 文本 / 矩形 / 图片 / 二维码（≤ 48 项，≤ 2 个二维码） |
| POST | `/api/v1/display/uploadFrame` | 原始 **960000** 字节原生帧（布局见下） |
| POST | `/api/v1/display/displayUrl` | `{url}` 设备自拉、解码并刷新（JPEG / PNG / raw；http 与 https）。跟随最多 5 次跳转（https 地址只跳到 https）。超过 700 KB 的 JPEG / PNG 在读正文之前拒绝（`image >700KB limit`）；文件越大解码得越小：1600×1200 的 JPEG 在约 400 KB 以内解到 400×300，再大就只有 200×150（正文还占着 PSRAM，RGB 缓冲只能用剩下的部分）。随后平滑放大并按整屏分辨率抖动——`imageQuality: quality` 时屏上每个像素一个墨点，`speed` 时 800×600（2×2 的点）；灰色内容只取黑白。更大的图或要求满画质时请发原生帧。其余响应体缓存在 PSRAM：名义上限 1280 KB。若单块不够（例如网页轮询留下的 TCP 控制块把堆切开），则分最多 3 段读入，JPEG/PNG 按回调顺序从分段缓冲解码；960000 字节原生帧同样走分段。仍装不下的文件在读正文前拒绝，屏幕保留上一画面（`file >1280KB limit`、`need …KB, got …KB in pieces`）。只支持基线 JPEG，渐进式 JPEG 报 `JPEG decode fail` |
| POST | `/api/v1/display/proxyImage` | `{url}` → 图片字节，供**网页预览** |
| GET | `/api/v1/display/getLastFrameMeta` | `{present,seq,source,savedUs,bytes,crc32}` |
| GET | `/api/v1/display/lastFrame` | 原始 **960000** 字节 native 4bpp（与 `uploadFrame` 同布局） |
| POST | `/api/v1/display/testPattern` | 测试图 |

### 原生帧布局

两颗驱动 IC 左右排布：帧 = **CS0 半区**（原生 x 0..599）接 **CS1 半区**（x 600..1199），各 1600 行 × 300 字节；每字节两个原生像素，高半字节在左。逻辑横屏 1600×1200 是该光栅逆时针旋转 90° 得到的（`lx = ny`，`ly = 1199 − nx`）。颜色码：黑 `0x0`、白 `0x1`、黄 `0x2`、红 `0x3`、蓝 `0x5`、绿 `0x6`。可用 [`tools/frame_convert.py`](../../tools/frame_convert.py) 生成。

### 当前屏（lastfrm）

成功的**用户**刷屏后（`uploadFrame`、`displayUrl` / 唤醒图、画布、文本……），设备把原生帧写入 Flash 分区 `lastfrm`（画面未变化时跳过写入）。SoftAP 配网页等系统画面不保存。SoftAP **概览**页展示快照；显示页「读取当前屏」拉入编辑预览。

### 网页推图（SoftAP 下推荐）

1. `proxyImage` / 本地文件 / `lastFrame` → 浏览器预览  
2. 浏览器量化 → `uploadFrame`

### 平台 / 唤醒推图

- MQTT `display_url` / 电源 `wakeUrl` → 设备侧 `displayUrl`。

## NeoMind MQTT

- 设备类型 `ek410_Eink`，Client ID `ek410-{deviceId}`
- Topic：`device/ek410_Eink/{deviceId}/uplink|downlink`
- 下行（`{"cmd":"...","params":{...}}`）：`ping` / `heartbeat`、`clear`、`render_text`、`render_canvas`、`display_url`、`reboot`、`sleep_now` / `sleep`、`set_power`。刷屏类指令先回 `queued`，完成后再回 `cmd_ack`；休眠无法开始时会补发 `error` 的 `cmd_ack`。
