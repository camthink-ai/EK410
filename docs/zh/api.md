# SoftAP / 局域网 HTTP API

基址：SoftAP `http://192.168.4.1`，或入网后的设备 STA IP。

多数接口直接返回 JSON 对象；错误形如 `{"code":1,"msg":"..."}`。

英文版：[api.md](../api.md)

## 系统 / 网络 / 显示

与英文版表格一致，要点：

- **网页推图**：`proxyImage` 预览 → 浏览器量化 → `uploadFrame`
- **当前屏**：成功用户刷屏后写入 Flash 分区 `lastfrm`（SoftAP 配网页等系统画面不保存）
  - SoftAP **概览**页展示快照；显示页「读取当前屏」拉入编辑预览
  - `GET /api/v1/display/getLastFrameMeta` → `{present,seq,source,savedUs,bytes,crc32}`
  - `GET /api/v1/display/lastFrame` → 原始 **960000** 字节 native 4bpp（与 `uploadFrame` 同布局）
- **平台 / 唤醒推图**：`displayUrl`（设备自拉自刷）
- **OTA**：仅 `https://`
- **MQTTS**：`tlsVerify`（默认 true）+ 可选 `caPem`（自定义 CA PEM；留空用系统 CA 包；关闭校验仅用于自签/内网）
- **NeoLink CT-Prov**：`POST /api/v1/ctprov`（SoftAP 绑定；SSID `EK410_<MAC6>`；详见 `docs/ct_prov.md`）
- **电源**：`always` / `cycle` / `hibernate`，`imageQuality`: `quality` | `speed`

## NeoMind

- 设备类型 `ek410_Eink`
- Client ID `ek410-{deviceId}`
- Topic：`device/ek410_Eink/{deviceId}/uplink|downlink`
