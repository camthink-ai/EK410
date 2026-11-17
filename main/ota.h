/**
 * @file ota.h
 * @brief HTTPS OTA upgrade with automatic rollback
 */
#ifndef __OTA_H__
#define __OTA_H__

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Download + flash the image at url (HTTPS, cert bundle). Blocking. */
int ota_start(const char *url, char *errBuf, int errLen);

/* Mark the running app as valid so the bootloader keeps it (rollback guard). */
void ota_confirm_running(void);

/* Running partition name + pending-rollback state for the status API. */
void ota_get_status(char *running, int runningLen, bool *pendingRollback);

/**
 * Dual-SKU boot switch (SoftAP image project_name "ek410", BLE "ek410_ble").
 * Sets next boot partition and returns ESP_OK. Caller restarts.
 */
esp_err_t ota_boot_softap_sku(void);
esp_err_t ota_boot_ble_sku(void);

/** True if a BLE SKU image is present on the other OTA slot. */
bool ota_ble_sku_available(void);

/** Running app project_name from esp_app_desc ("ek410" / "ek410_ble"). */
const char *ota_running_project_name(void);

#ifdef __cplusplus
}
#endif

#endif /* __OTA_H__ */
