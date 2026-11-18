# Changelog

All notable changes to the CamThink **EK410** firmware are documented here.

Format follows [Keep a Changelog](https://keepachangelog.com/). Versioning follows [Semantic Versioning](https://semver.org/).

中文说明见各版本条目下的 **中文** 小节。

---

## [Unreleased]

### Changed

- Single firmware image: SoftAP XOR BLE CT-Prov in-process (no dual OTA SKU / reboot switch).
- NimBLE enabled in `sdkconfig.defaults`; PAGE/MAIN toggle NimBLE without changing boot slot.

### 中文

- 单镜像：SoftAP 与 BLE CT-Prov 进程内互斥启停，不再切换 OTA SKU。

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
