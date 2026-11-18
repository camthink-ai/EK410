# CamThink EK410 — Spectra 6 e-ink firmware (ESP32-C61, 1600x1200)
# Version: see VERSION / main/version.h (python3 tools/bump_version.py)
#
# Single image: SoftAP XOR BLE CT-Prov in-process (never both up).
cmake_minimum_required(VERSION 3.16)

set(SDKCONFIG_DEFAULTS "${CMAKE_SOURCE_DIR}/sdkconfig.defaults")
set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(ek410)
