/**
 * @file main.c
 * @brief CamThink EK410 E-Ink Display firmware
 *
 * Boot flow:
 *   config_open -> panel init -> last frame (or CamThink logo if empty)
 *   -> NVS has Wi-Fi ? STA connect (SoftAP off) : SoftAP off
 *   -> SoftAP via MAIN hold 2.5s (blue); BLE CT-Prov via PAGE hold 2.5s (cyan);
 *      SoftAP XOR BLE; first SoftAP paints Wi-Fi guide
 *   -> http_server_start; mqtt on got-ip; sleep cycle arms after online
 *
 * Hardware RESET / power-on: exit BLE/SoftAP handoff, restore last frame.
 * Software restart (SKU switch): keep bleboot / softap_pending.
 *
 * Hold CHIP BOOT (IO9) during reset = factory reset (uid preserved).
 */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "sdkconfig.h"
#include "driver/gpio.h"

#include "config.h"
#include "wifi.h"
#include "mqtt.h"
#include "http.h"
#include "ota.h"
#include "sleep.h"
#include "led.h"
#include "button.h"
#include "app_epd.h"
#include "battery.h"
#include "last_frame.h"
#include "pindefine.h"
#include "ota.h"

#define TAG "-->MAIN"

static bool boot_button_held(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BTN_BOOT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);
    vTaskDelay(pdMS_TO_TICKS(50));
    return gpio_get_level(BTN_BOOT_GPIO) == 0;
}

/** SoftAP↔BLE handoff uses esp_restart(); HW RESET / power-on must drop prov. */
static bool boot_keep_prov_handoff(void)
{
    return esp_reset_reason() == ESP_RST_SW;
}

static void boot_paint_panel(void)
{
    if (app_epd_last_frame_async() == 0) {
        ESP_LOGI(TAG, "panel: restoring last frame");
        return;
    }
    if (app_epd_splash_async() != 0) {
        ESP_LOGW(TAG, "splash queue failed");
    }
}

void app_main(void)
{
    config_open();
    led_open();

    /* Dual SKU: NimBLE-enabled build is the BLE image (no SoftAP httpd). */
#if CONFIG_BT_NIMBLE_ENABLED
    const bool ble_sku = true;
#else
    const bool ble_sku = false;
    if (cfg_ble_prov_boot_get()) {
        cfg_ble_prov_boot_set(false);
    }
    /* Drop CI idle override left in NVS after SoftAP↔BLE switches. */
    cfg_ble_idle_sec_set(0);
#endif

    const bool keep_handoff = boot_keep_prov_handoff();
    if (!keep_handoff) {
        /* Hardware RESET / power-on / WDT / panic: leave provisioning. */
        ESP_LOGI(TAG, "reset reason %d — clear SoftAP/BLE handoff",
                 (int)esp_reset_reason());
        cfg_softap_pending_set(false);
        cfg_ble_prov_boot_set(false);
        cfg_ble_idle_sec_set(0);
    }

    if (ble_sku) {
        if (!keep_handoff) {
            /* HW reset landed on BLE OTA slot — boot SoftAP SKU instead. */
            ESP_LOGW(TAG, "HW reset on BLE SKU — switch to SoftAP + restart");
            led_set(LEDC_NET_R, LED_BLINK);
            if (ota_boot_softap_sku() != ESP_OK) {
                ESP_LOGE(TAG, "SoftAP SKU missing — stay on BLE without NimBLE");
            } else {
                vTaskDelay(pdMS_TO_TICKS(200));
                esp_restart();
            }
        }
        led_set(LEDC_BLE_R, LED_BLINK); /* cyan: BLE SKU */
        cfg_ble_prov_boot_set(true);
        /* Skip last_frame + panel on BLE SKU — free internal RAM for NimBLE+Wi-Fi scan. */
    } else {
        last_frame_init();
        led_set(LEDC_NET_R, LED_BLINK); /* blue: SoftAP SKU bring-up */
    }

    if (boot_button_held()) {
        ESP_LOGW(TAG, "BOOT held, factory reset");
        cfg_factory_reset();
        cfg_ble_prov_boot_set(false);
        cfg_softap_pending_set(false);
        cfg_ble_idle_sec_set(0);
    }

    if (!ble_sku) {
        if (app_epd_init() != 0) {
            ESP_LOGE(TAG, "panel init failed; continuing headless");
        }
    }

    wifi_open();
    button_open();

    bool online = false;
    bool hasWifi = config_has_wifi();

    if (ble_sku) {
        /* NimBLE before Wi-Fi driver + MQTT — C61 largest_int collapses after. */
        wifi_ble_prov_enter();
        ESP_LOGI(TAG, "BLE SKU (%s) — PAGE/idle/session.close → SoftAP SKU",
                 ota_running_project_name());
        mqtt_open();
        /* Defer STA while BLE window is up — scan/connect on demand via CT-Prov. */
    } else {
        mqtt_open();
        http_server_start();
        if (hasWifi) {
            led_set(LEDC_NET_R, LED_BLINK);
            online = wifi_sta_start();
        }
        /* SoftAP only after intentional SoftAP↔BLE software handoff. */
        if (keep_handoff && cfg_softap_pending_get()) {
            cfg_softap_pending_set(false);
            ESP_LOGI(TAG, "SoftAP pending after BLE exit — opening");
            wifi_softap_open();
        }
    }

    if (!ble_sku) {
        boot_paint_panel();
    }

    if (ble_sku) {
        /* cyan from wifi_ble_prov_enter */
    } else {
        /* Steady LED from Wi-Fi/SoftAP state (not panel probe — avoids red after BLE idle). */
        wifi_led_refresh();
        if (online) {
            sleep_on_sta_online();
            if (!app_epd_panel_ok()) {
                /* Log only — never red-pulse here (looks like BLE/SoftAP fault). */
                ESP_LOGW(TAG, "panel probe unhealthy — LED stays green while STA up");
            }
        } else if (hasWifi) {
            ESP_LOGI(TAG, "Wi-Fi saved but not yet online — reconnect armed");
        } else {
            ESP_LOGI(TAG, "hold MAIN 2.5s=SoftAP, PAGE 2.5s=BLE SKU (IO%d/IO%d)",
                     BTN_MAIN_GPIO, BTN_PAGE_GPIO);
        }
    }

    /* Confirm OTA so dual-SKU slot switches are not rolled back.
     * SoftAP: confirm when httpd is up (panel may still be unhealthy).
     * BLE SKU: always confirm. */
    if (ble_sku || http_server_is_up() || app_epd_panel_ok()) {
        ota_confirm_running();
    } else {
        ESP_LOGW(TAG, "deferring OTA confirm — no httpd/panel yet");
    }

    {
        int mv = battery_read_mv();
        if (mv >= 0) {
            ESP_LOGI(TAG, "battery %d mV (~%d%%)", mv, battery_soc_from_mv(mv));
        } else {
            ESP_LOGW(TAG, "battery sense unavailable");
        }
    }
    ESP_LOGI(TAG, "running");
}
