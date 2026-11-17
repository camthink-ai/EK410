# CamThink EK410 — Spectra 6 e-ink firmware (ESP32-C61, 1600x1200)
# Version: see VERSION / main/version.h (python3 tools/bump_version.py)
#
# Dual SKU (C61 RAM cannot host SoftAP httpd + NimBLE together):
#   SoftAP:  idf.py -B build build              → project ek410
#   BLE:     EK410_BLE_BUILD=1 idf.py -B build_ble build → project ek410_ble
#
# Each -B build dir gets its own sdkconfig so SoftAP/BLE defaults do not clash.
cmake_minimum_required(VERSION 3.16)

if(DEFINED ENV{EK410_BLE_BUILD})
    set(SDKCONFIG_DEFAULTS "${CMAKE_SOURCE_DIR}/sdkconfig.defaults;${CMAKE_SOURCE_DIR}/sdkconfig.ble_prov")
    set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig")
else()
    set(SDKCONFIG_DEFAULTS "${CMAKE_SOURCE_DIR}/sdkconfig.defaults")
    set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

if(DEFINED ENV{EK410_BLE_BUILD})
    project(ek410_ble)
else()
    project(ek410)
endif()
