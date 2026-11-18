/**
 * @file ct_prov_ble.h
 * @brief CT-Prov/1 NimBLE peripheral (NeoLink GATT UUIDs + fragment layer).
 *
 * Lifecycle is owned by wifi_ble_prov_open/close (PAGE hold window).
 * stop() fully deinits NimBLE so SoftAP/httpd can reclaim RAM.
 * Built only when CONFIG_BT_NIMBLE_ENABLED=y.
 */
#ifndef CT_PROV_BLE_H_
#define CT_PROV_BLE_H_

#include "esp_err.h"
#include <stdbool.h>
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

#if CONFIG_BT_NIMBLE_ENABLED

/** Start NimBLE host + advertise CT-Prov (idempotent). */
esp_err_t ct_prov_ble_start(void);

/** Create GATT worker after BLE-warm Wi-Fi (keeps internal RAM for wifi_init). */
esp_err_t ct_prov_ble_start_worker(void);

/** Stop advertising, disconnect, and fully deinit NimBLE (idempotent). */
void ct_prov_ble_stop(void);

bool ct_prov_ble_is_up(void);

/** True while a CT-Prov GATT client is connected (BLE idle uses this). */
bool ct_prov_ble_client_connected(void);

/** Pause advertising during Wi-Fi associate (keeps GATT link). */
void ct_prov_ble_rf_quiet(void);

/** Resume advertising after Wi-Fi associate attempt. */
void ct_prov_ble_rf_restore(void);

#else

static inline esp_err_t ct_prov_ble_start(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

static inline esp_err_t ct_prov_ble_start_worker(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

static inline void ct_prov_ble_stop(void) {}

static inline bool ct_prov_ble_is_up(void)
{
    return false;
}

static inline bool ct_prov_ble_client_connected(void)
{
    return false;
}

static inline void ct_prov_ble_rf_quiet(void) {}

static inline void ct_prov_ble_rf_restore(void) {}

#endif /* CONFIG_BT_NIMBLE_ENABLED */

#ifdef __cplusplus
}
#endif

#endif /* CT_PROV_BLE_H_ */
