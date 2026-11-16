/**
 * @file main.c
 * @brief CamThink EK410 E-Ink Display firmware
 *
 * Boot flow:
 *   config_open -> panel init -> CamThink logo (empty panel)
 *   -> NVS has Wi-Fi ? STA connect (SoftAP off) : SoftAP off
 *   -> SoftAP only via long-press MAIN 2.5s; first-time paints Wi-Fi guide,
 *      SoftAP close restores logo if guide was shown
 *   -> http_server_start; mqtt on got-ip; sleep cycle arms after online
 *
 * Hold CHIP BOOT (IO9) during reset = factory reset (uid preserved).
 */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
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
#include "pindefine.h"

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

void app_main(void)
{
    config_open();
    led_open();
    led_set(LEDC_NET_R, LED_BLINK); /* blue: bringing network up */

    if (boot_button_held()) {
        ESP_LOGW(TAG, "BOOT held, factory reset");
        cfg_factory_reset();
    }

    if (app_epd_init() != 0) {
        ESP_LOGE(TAG, "panel init failed; continuing headless");
    }

    mqtt_open();
    wifi_open();
    /* Start button before slow panel draws so SoftAP long-press works ASAP. */
    button_open();
    http_server_start();

    bool online = false;
    bool hasWifi = config_has_wifi();
    if (hasWifi) {
        led_set(LEDC_NET_R, LED_BLINK); /* blue: joining home Wi-Fi */
        online = wifi_sta_start();
    }

    /* Empty panel = CamThink logo. Wi-Fi setup QR only via SoftAP open. */
    if (app_epd_splash_async() != 0) {
        ESP_LOGW(TAG, "splash queue failed");
    }

    if (online) {
        sleep_on_sta_online();
        if (!app_epd_panel_ok()) {
            led_set(LEDC_ERR_R, LED_BLINK); /* red: panel fault */
        } else {
            led_set(LEDC_OK_R, LED_ON); /* solid green: running */
        }
    } else if (hasWifi) {
        if (!wifi_softap_is_up()) {
            led_set(LEDC_NET_R, LED_BLINK);
        }
        ESP_LOGI(TAG, "Wi-Fi saved but not yet online — reconnect armed");
    } else {
        if (!wifi_softap_is_up()) {
            led_set(LEDC_OK_R, LED_ON);
        }
        ESP_LOGI(TAG, "SoftAP off — hold MAIN (IO%d) 2.5s for setup AP",
                 BTN_MAIN_GPIO);
    }

    /* Only commit OTA slot when the panel reports healthy. */
    if (app_epd_panel_ok()) {
        ota_confirm_running();
    } else {
        ESP_LOGW(TAG, "deferring OTA confirm — panel not healthy");
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
