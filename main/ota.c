/**
 * @file ota.c
 * @brief HTTPS OTA with anti-rollback (ota_0 / ota_1).
 *
 * partitions.csv ships ota_0/ota_1 + otadata; sdkconfig enables
 * BOOTLOADER_APP_ROLLBACK. A freshly flashed image boots as "pending valid";
 * ota_confirm_running() is called once the system self-check passes (web
 * server up). If it never runs, the bootloader reverts to the previous slot
 * on next reset — a failed update can never brick the device.
 *
 * SoftAP and BLE CT-Prov share one firmware image (in-process XOR).
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_app_format.h"
#include "esp_https_ota.h"
#include "esp_crt_bundle.h"
#include "esp_partition.h"

#include "ota.h"

#define TAG "-->OTA"

int ota_start(const char *url, char *errBuf, int errLen)
{
    esp_http_client_config_t httpCfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
    };
    esp_https_ota_config_t otaCfg = {
        .http_config = &httpCfg,
    };

    esp_err_t err = esp_https_ota(&otaCfg);
    if (err == ESP_OK) {
        ESP_LOGW(TAG, "OTA OK, rebooting into new image");
        return 0;
    }
    ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(err));
    snprintf(errBuf, errLen, "%s", esp_err_to_name(err));
    return -1;
}

void ota_confirm_running(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    if (esp_ota_get_state_partition(running, &state) == ESP_OK) {
        if (state == ESP_OTA_IMG_PENDING_VERIFY) {
            esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
            ESP_LOGW(TAG, "pending-verify image confirmed (%s)",
                     err == ESP_OK ? "valid" : esp_err_to_name(err));
        }
    }
}

void ota_get_status(char *running, int runningLen, bool *pendingRollback)
{
    const esp_partition_t *part = esp_ota_get_running_partition();
    strlcpy(running, part ? part->label : "?", runningLen);
    *pendingRollback = false;
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    if (part && esp_ota_get_state_partition(part, &state) == ESP_OK) {
        *pendingRollback = (state == ESP_OTA_IMG_PENDING_VERIFY);
    }
}

const char *ota_running_project_name(void)
{
    const esp_app_desc_t *desc = esp_app_get_description();
    return desc ? desc->project_name : "?";
}
