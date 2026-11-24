/**
 * @file ota.c
 * @brief HTTPS OTA + local upload into ota_0/ota_1 with anti-rollback.
 *
 * partitions.csv ships ota_0/ota_1 + otadata; sdkconfig enables
 * BOOTLOADER_APP_ROLLBACK. A freshly flashed image boots as "pending valid";
 * ota_confirm_running() is called once the system self-check passes (web
 * server up). If it never runs, the bootloader reverts to the previous slot
 * on next reset — a failed update can never brick the device.
 *
 * SoftAP and BLE CT-Prov share one firmware image (in-process XOR).
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_app_format.h"
#include "esp_https_ota.h"
#include "esp_crt_bundle.h"
#include "esp_partition.h"

#include "ota.h"
#include "wifi.h"
#include "app_epd.h"
#include "led.h"
#include "mqtt.h"
#include "sleep.h"

#define TAG "-->OTA"
#define OTA_IMAGE_MAGIC 0xE9

static SemaphoreHandle_t s_otaMu;
static esp_ota_handle_t s_otaHandle;
static const esp_partition_t *s_otaPart;
static size_t s_otaWritten;
static bool s_otaActive;
static bool s_otaSawMagic;
/* URL OTA downloads for minutes while holding s_otaMu: tracked separately so
 * mqtt.c and the upload handler can ask "is flash being written" lock-free. */
static volatile bool s_urlOtaRunning;
static volatile ota_state_t s_otaState;
static char s_otaErr[64];

static void ota_set_fail(const char *err)
{
    strlcpy(s_otaErr, (err && err[0]) ? err : "failed", sizeof(s_otaErr));
    s_otaState = OTA_STATE_FAILED;
}

static void ota_set_running(void)
{
    s_otaErr[0] = '\0';
    s_otaState = OTA_STATE_RUNNING;
}

bool ota_busy(void)
{
    return s_otaActive || s_urlOtaRunning;
}

bool ota_url_claim(void)
{
    if (ota_busy()) {
        return false;
    }
    s_urlOtaRunning = true;
    ota_set_running();
    return true;
}

void ota_url_release(const char *err)
{
    s_urlOtaRunning = false;
    if (err && err[0]) {
        ota_set_fail(err);
    } else {
        s_otaState = OTA_STATE_IDLE;
    }
}

void ota_restore_runtime(void)
{
    wifi_led_refresh();
    mqtt_restore_after_temp_stop();
    sleep_kick();
}

void ota_url_run(const char *url)
{
    char err[48] = "";
    if (!url || !url[0]) {
        ota_url_release("no url");
        ota_restore_runtime();
        return;
    }
    ESP_LOGW(TAG, "OTA start: %s", url);
    led_set(LEDC_OTA_R, LED_BLINK_FAST); /* purple: upgrading */
    int rc = ota_start(url, err, sizeof(err));
    if (rc == 0) {
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    }
    ESP_LOGE(TAG, "OTA failed: %s", err);
    led_pulse(LEDC_ERR_R, 2000);
    /* Publish the failure before MQTT comes back: the web UI polls otaState. */
    ota_url_release(err[0] ? err : "download failed");
    ota_restore_runtime();
}

ota_state_t ota_state(void)
{
    return s_otaState;
}

const char *ota_state_str(void)
{
    return (s_otaState == OTA_STATE_RUNNING) ? "running"
           : (s_otaState == OTA_STATE_FAILED) ? "failed"
                                              : "idle";
}

const char *ota_last_error(void)
{
    return s_otaErr;
}

void ota_clear_error(void)
{
    if (s_otaState == OTA_STATE_FAILED) {
        s_otaState = OTA_STATE_IDLE;
        s_otaErr[0] = '\0';
    }
}

static void ota_mu_ensure(void)
{
    if (!s_otaMu) {
        s_otaMu = xSemaphoreCreateMutex();
    }
}

int ota_start(const char *url, char *errBuf, int errLen)
{
    if (errBuf && errLen > 0) {
        errBuf[0] = '\0';
    }
    ota_mu_ensure();
    if (!s_otaMu || xSemaphoreTake(s_otaMu, pdMS_TO_TICKS(100)) != pdTRUE) {
        if (errBuf) {
            snprintf(errBuf, errLen, "ota busy");
        }
        return -1;
    }
    if (s_otaActive) {
        xSemaphoreGive(s_otaMu);
        if (errBuf) {
            snprintf(errBuf, errLen, "upload in progress");
        }
        return -1;
    }

    esp_http_client_config_t httpCfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
    };
    esp_https_ota_config_t otaCfg = {
        .http_config = &httpCfg,
    };

    esp_err_t err = esp_https_ota(&otaCfg);
    xSemaphoreGive(s_otaMu);
    if (err == ESP_OK) {
        ESP_LOGW(TAG, "URL OTA OK, rebooting into new image");
        return 0;
    }
    ESP_LOGE(TAG, "URL OTA failed: %s", esp_err_to_name(err));
    if (errBuf) {
        snprintf(errBuf, errLen, "%s", esp_err_to_name(err));
    }
    return -1;
}

int ota_upload_begin(size_t image_len, char *errBuf, int errLen)
{
    if (errBuf && errLen > 0) {
        errBuf[0] = '\0';
    }
    ota_mu_ensure();
    if (!s_otaMu || xSemaphoreTake(s_otaMu, pdMS_TO_TICKS(500)) != pdTRUE) {
        if (errBuf) {
            snprintf(errBuf, errLen, "ota busy");
        }
        return -1;
    }
    if (s_otaActive) {
        xSemaphoreGive(s_otaMu);
        if (errBuf) {
            snprintf(errBuf, errLen, "upload already active");
        }
        return -1;
    }

    const esp_partition_t *update = esp_ota_get_next_update_partition(NULL);
    if (!update) {
        xSemaphoreGive(s_otaMu);
        if (errBuf) {
            snprintf(errBuf, errLen, "no OTA partition");
        }
        return -1;
    }
    if (image_len > 0 && image_len > update->size) {
        xSemaphoreGive(s_otaMu);
        if (errBuf) {
            snprintf(errBuf, errLen, "image larger than OTA slot (%u)",
                     (unsigned)update->size);
        }
        return -1;
    }

    size_t begin_size = image_len > 0 ? image_len : OTA_SIZE_UNKNOWN;
    esp_err_t err = esp_ota_begin(update, begin_size, &s_otaHandle);
    if (err != ESP_OK) {
        xSemaphoreGive(s_otaMu);
        ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(err));
        if (errBuf) {
            snprintf(errBuf, errLen, "begin %s", esp_err_to_name(err));
        }
        return -1;
    }

    s_otaPart = update;
    s_otaWritten = 0;
    s_otaSawMagic = false;
    s_otaActive = true;
    ota_set_running();
    ESP_LOGI(TAG, "upload begin → %s offset=0x%lx size=%u (expect %u)",
             update->label, (unsigned long)update->address,
             (unsigned)update->size, (unsigned)image_len);
    xSemaphoreGive(s_otaMu);
    return 0;
}

int ota_upload_write(const void *data, size_t len)
{
    if (!data || len == 0) {
        return -1;
    }
    ota_mu_ensure();
    if (!s_otaMu || xSemaphoreTake(s_otaMu, pdMS_TO_TICKS(5000)) != pdTRUE) {
        return -1;
    }
    if (!s_otaActive) {
        xSemaphoreGive(s_otaMu);
        return -1;
    }

    const uint8_t *p = (const uint8_t *)data;
    if (!s_otaSawMagic) {
        if (p[0] != OTA_IMAGE_MAGIC) {
            xSemaphoreGive(s_otaMu);
            ESP_LOGE(TAG, "bad image magic 0x%02x (want 0xE9)", p[0]);
            return -1;
        }
        s_otaSawMagic = true;
    }

    esp_err_t err = esp_ota_write(s_otaHandle, data, len);
    if (err != ESP_OK) {
        xSemaphoreGive(s_otaMu);
        ESP_LOGE(TAG, "esp_ota_write @%u: %s", (unsigned)s_otaWritten,
                 esp_err_to_name(err));
        return -1;
    }
    s_otaWritten += len;
    xSemaphoreGive(s_otaMu);
    return 0;
}

void ota_upload_abort(void)
{
    ota_mu_ensure();
    if (!s_otaMu) {
        return;
    }
    /* A stuck active flag would refuse every later OTA until reboot, so keep
     * trying: finish() holds the mutex for seconds while it validates. */
    if (xSemaphoreTake(s_otaMu, pdMS_TO_TICKS(10000)) != pdTRUE) {
        ESP_LOGE(TAG, "upload abort: mutex busy for 10 s — OTA stays busy until reboot");
        return;
    }
    if (s_otaActive) {
        esp_ota_abort(s_otaHandle);
        ESP_LOGW(TAG, "upload aborted after %u bytes", (unsigned)s_otaWritten);
        s_otaActive = false;
        s_otaHandle = 0;
        s_otaPart = NULL;
        s_otaWritten = 0;
        s_otaSawMagic = false;
        ota_set_fail("upload aborted");
    }
    xSemaphoreGive(s_otaMu);
}

int ota_upload_finish(char *errBuf, int errLen)
{
    if (errBuf && errLen > 0) {
        errBuf[0] = '\0';
    }
    ota_mu_ensure();
    if (!s_otaMu || xSemaphoreTake(s_otaMu, pdMS_TO_TICKS(10000)) != pdTRUE) {
        if (errBuf) {
            snprintf(errBuf, errLen, "ota busy");
        }
        return -1;
    }
    if (!s_otaActive || !s_otaPart) {
        xSemaphoreGive(s_otaMu);
        if (errBuf) {
            snprintf(errBuf, errLen, "no active upload");
        }
        return -1;
    }

    esp_err_t err = esp_ota_end(s_otaHandle);
    if (err != ESP_OK) {
        /* end() already aborts the handle on failure in IDF 5.x */
        s_otaActive = false;
        s_otaHandle = 0;
        ESP_LOGE(TAG, "esp_ota_end after %u bytes: %s",
                 (unsigned)s_otaWritten, esp_err_to_name(err));
        if (errBuf) {
            snprintf(errBuf, errLen, "validate %s", esp_err_to_name(err));
        }
        s_otaPart = NULL;
        s_otaWritten = 0;
        s_otaSawMagic = false;
        ota_set_fail(errBuf);
        xSemaphoreGive(s_otaMu);
        return -1;
    }

    err = esp_ota_set_boot_partition(s_otaPart);
    if (err != ESP_OK) {
        s_otaActive = false;
        s_otaHandle = 0;
        ESP_LOGE(TAG, "set_boot_partition: %s", esp_err_to_name(err));
        if (errBuf) {
            snprintf(errBuf, errLen, "boot %s", esp_err_to_name(err));
        }
        s_otaPart = NULL;
        s_otaWritten = 0;
        s_otaSawMagic = false;
        ota_set_fail(errBuf);
        xSemaphoreGive(s_otaMu);
        return -1;
    }

    ESP_LOGW(TAG, "upload OK %u bytes → boot %s (pending-verify until confirm)",
             (unsigned)s_otaWritten, s_otaPart->label);
    s_otaActive = false;
    s_otaHandle = 0;
    s_otaPart = NULL;
    s_otaWritten = 0;
    s_otaSawMagic = false;
    xSemaphoreGive(s_otaMu);
    return 0;
}

bool ota_upload_is_active(void)
{
    return s_otaActive;
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

void ota_selfcheck(bool web_up)
{
    /* A still-pending image is rolled back on the next reset, and a CYCLE
     * unit may be up for only ~30 s before it sleeps — so no grace timers:
     * a healthy unit must pass right here, on its first reachability. */
    if (!web_up && !wifi_sta_is_connected()) {
        return;
    }
    if (!app_epd_panel_ok() && !web_up) {
        return; /* headless unit: confirm once the web server is reachable */
    }
    ota_confirm_running();
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
