# Changelog

All notable changes to the CamThink **EK410** firmware are documented here.

Format follows [Keep a Changelog](https://keepachangelog.com/). Versioning follows [Semantic Versioning](https://semver.org/).

中文说明见各版本条目下的 **中文** 小节。

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
