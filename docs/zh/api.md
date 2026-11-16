# SoftAP / 局域网 HTTP API

基址：SoftAP `http://192.168.4.1`，或入网后的设备 STA IP。

多数接口直接返回 JSON 对象；错误形如 `{"code":1,"msg":"..."}`。

英文版：[api.md](../api.md)

## 系统 / 网络 / 显示

与英文版表格一致，要点：

- **网页推图**：`proxyImage` 预览 → 浏览器量化 → `uploadFrame`
- **平台 / 唤醒推图**：`displayUrl`（设备自拉自刷）
- **OTA**：仅 `https://`
- **电源**：`always` / `cycle` / `hibernate`，`imageQuality`: `quality` | `speed`

## NeoMind

- 设备类型 `ek410_Eink`
- Client ID `ek410-{deviceId}`
- Topic：`device/ek410_Eink/{deviceId}/uplink|downlink`
