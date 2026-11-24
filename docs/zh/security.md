# 安全说明

源码：[github.com/camthink-ai/EK410](https://github.com/camthink-ai/EK410)

对应固件 1.3.10。在把设备部署到实验室之外、或进行首次量产烧录之前请先阅读。

## 已有的保护

- **HTTPS OTA** 使用内置 CA 列表校验服务器证书，只接受 `https://`，并拒绝降级到 `http://` 的重定向。
- **HTTPS 拉图**（`displayUrl`、`wakeUrl`、`proxyImage`）同样校验服务器证书。允许 `http://` 图片地址，其内容没有完整性保护，但解码有尺寸与边界检查。
- **MQTTS** 默认校验证书（`tlsVerify`：内置 CA 或自定义 CA PEM）。「不校验」会关闭验证，网页上已标注为不安全。
- **BLE CT-Prov** 的 GATT 特征值需要加密连接（配对）后才能读写。
- **密码经 API 只写不读**：`getWifiParam` 与 `getMqttParam` 只返回 `hasPassword`，不返回密码。
- **输入限制**：HTTP 请求体 ≤ 16 KB，CT-Prov 请求体 ≤ 4 KB，JSON 嵌套 ≤ 8 层，图片尺寸与分配大小有上限。

## 当前设计未保护的部分

- **HTTP API 没有任何鉴权。** 同一套 32 个接口同时开放在 SoftAP（`192.168.4.1`）和局域网（STA IP）上。任何能访问到设备的终端都可以修改 Wi‑Fi 与 MQTT 配置（包括把 MQTT 指向别的 broker）、刷入任意固件（`uploadFirmware`、`setDevUpgrade`）、恢复出厂、重启、强制深睡、推送图片、读取事件日志与设备信息。
- **SoftAP 密码固定为 `12345678`**，所有设备相同。
- **固件镜像没有签名**（未启用 Secure Boot），**Flash / NVS 未加密**：有物理接触即可读出保存的凭据；OTA 的信任完全依赖 TLS 服务器认证，以及不可信者接触不到上传接口这一前提。
- **BLE 配对为 Just-Works**（无中间人保护）：BLE 窗口打开时，附近任何手机都能配网。
- **MQTT 下行指令直接执行。** 能向设备下行主题发布消息的一方即可控制设备（`reboot`、`sleep_now`、`set_power`、`display_url` 等）。

## 量产前需要决定的事项

1. **接口鉴权。** 最低要求：破坏性接口需要设备级令牌（印在标签上或显示在 SoftAP 页面）——`uploadFirmware`、`setDevUpgrade`、`setDevFactory`、`setDevReset`、`setWifiParam`、`setMqttParam`、`setPowerParam`、`sleepNow`、`openSoftAp`、`openBleProv`，以及 CT-Prov 的 `config.set`。
2. **每台设备独立的 SoftAP 密码**（由设备 id 派生，显示在配网画面上），取代 `12345678`。
3. **Secure Boot V2 签名固件、Flash 加密、NVS 加密。** 这些会烧写 eFuse、需要密钥管理和产线烧录环节，且无法远程追加到已出货设备上——必须在首次量产烧录之前定下来。
4. 第 3 项就位后**锁定 UART 下载模式**（eFuse）。
5. 在第 1–3 项完成之前，把设备部署在隔离的 LAN / VLAN / SSID 中：局域网接口暴露的未鉴权 API 与热点完全相同。
