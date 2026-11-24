/**
 * @file main.c
 * @brief CamThink EK410 E-Ink Display firmware
 *
 * Boot flow:
 *   config_open -> panel init -> last frame (or CamThink logo if empty)
 *   -> NVS has Wi-Fi ? STA connect (SoftAP off) : SoftAP off
 *   -> SoftAP via MAIN hold 2.5s (blue); BLE CT-Prov via PAGE hold 2.5s (cyan);
 *      SoftAP XOR BLE in one image (never both up)
 *   -> Deep sleep wake: MAIN → SoftAP, PAGE → BLE, timer → cycle
 *   -> http_server_start; mqtt on got-ip; sleep cycle arms after online
 *
 * Hardware RESET / power-on: exit SoftAP/BLE provisioning, restore last frame.
 *
 * Hold CHIP BOOT (IO9) during reset = factory reset (uid preserved).
 */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "cJSON.h"

#include "config.h"
#include "wifi.h"
#include "mqtt.h"
#include "http.h"
#include "ota.h"
#include "sleep.h"
#include "led.h"
#include "button.h"
#include "app_epd.h"
#include "evtlog.h"
#include "battery.h"
#include "last_frame.h"
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

static void wake_softap_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(200));
    ESP_LOGW(TAG, "deep-sleep MAIN wake → SoftAP");
    led_set(LEDC_NET_R, LED_BLINK);
    wifi_softap_open();
    vTaskDelete(NULL);
}

static void wake_ble_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(200));
    ESP_LOGW(TAG, "deep-sleep PAGE wake → BLE");
    led_set(LEDC_BLE_R, LED_BLINK);
    wifi_ble_prov_open();
    vTaskDelete(NULL);
}

static void boot_apply_sleep_wake_provisioning(void)
{
    sleep_wake_src_t w = sleep_consume_wake_src();
    if (w == SLEEP_WAKE_MAIN) {
        if (xTaskCreate(wake_softap_task, "wake_ap", 8192, NULL, 5, NULL) != pdPASS) {
            wifi_softap_open();
        }
    } else if (w == SLEEP_WAKE_PAGE) {
        if (xTaskCreate(wake_ble_task, "wake_ble", 8192, NULL, 5, NULL) != pdPASS) {
            wifi_ble_prov_open();
        }
    }
}

/**
 * cJSON nodes are ~64 bytes and, with CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=1024,
 * every one of them (and every short key string) comes from internal RAM. A
 * getDevInfo reply is ~60 nodes (~5 KB): with the internal heap this unit has
 * beside Wi-Fi, httpd and MQTT, a reply that carried diagnostics used to take
 * the free internal RAM down to single-digit bytes. Prefer PSRAM for
 * everything cJSON allocates; the call falls back to internal RAM when PSRAM
 * is exhausted.
 */
static void *json_alloc(size_t n)
{
    /* PSRAM first; internal RAM only once PSRAM is exhausted. */
    return n ? heap_caps_malloc_prefer(n, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                       MALLOC_CAP_8BIT)
             : NULL;
}

static void json_mem_init(void)
{
    /* Before the first cJSON user. IDF's cJSON has no realloc hook: with
     * custom hooks it grows its print buffers through allocate + memcpy. */
    cJSON_Hooks hooks = { .malloc_fn = json_alloc, .free_fn = free };
    cJSON_InitHooks(&hooks);
}

void app_main(void)
{
    json_mem_init();
    config_open();
    led_open();
    sleep_capture_wake_cause();

    /* Drop stale provisioning flags (HW reset / leftover NVS).
     * Keep deep-sleep button wake intent (RAM only, not NVS). */
    ESP_LOGI(TAG, "reset reason %d", (int)esp_reset_reason());
    if (esp_reset_reason() != ESP_RST_SW) {
        cfg_softap_pending_set(false);
        cfg_ble_prov_boot_set(false);
        cfg_ble_idle_sec_set(0);
    }
    cfg_ble_idle_sec_set(0);

    last_frame_init();
    evtlog_init();
    evtlog_event("BOOT", "reset=%d wake=%d", (int)esp_reset_reason(),
                 (int)sleep_get_wake_src());
    /* Keep LED off during long panel paint (~40s). Blue blink only means
     * SoftAP / STA joining — do not hold NET blue for the whole boot paint. */
    led_set(0, 0, 0, LED_OFF);

    if (boot_button_held()) {
        ESP_LOGW(TAG, "BOOT held, factory reset");
        evtlog_event("BOOT", "factory via BOOT pin");
        cfg_factory_reset();
        cfg_ble_prov_boot_set(false);
        cfg_softap_pending_set(false);
        cfg_ble_idle_sec_set(0);
        /* Match button/HTTP factory paths — wipe RAM tasks/MQTT too. */
        esp_restart();
    }

    if (app_epd_init() != 0) {
        ESP_LOGE(TAG, "panel init failed; continuing headless");
    } else {
        /* Must paint before Wi-Fi: SPI DMA fails once NimBLE+Wi-Fi hold RAM
         * (dirty / incomplete lastfrm restore). */
        (void)app_epd_boot_paint_sync();
    }

    wifi_open();
    /* Prov worker first while largest_int is still high. */
    button_open();
    /* mqtt_cmd stack before wifi_start — keep small (see mqtt_open). */
    mqtt_open();

    bool online = false;
    bool hasWifi = config_has_wifi();
    if (hasWifi) {
        led_set(LEDC_JOIN_R, LED_BLINK);
        /* Start STA before httpd so esp_wifi_start can create PHY locks.
         * GOT_IP brings httpd then mqtt; we reinforce the same order here. */
        online = wifi_sta_start();
    }
    /* After a failed STA join, Wi-Fi already holds most internal RAM —
     * forcing httpd here yields ESP_ERR_HTTPD_TASK and leaves largest_int
     * too small for the later GOT_IP mqtt start (phy NO_MEM abort). Defer
     * to GOT_IP / httpd watchdog when STA is still joining. */
    if (online || !hasWifi) {
        http_server_start();
        if (!http_server_is_up()) {
            vTaskDelay(pdMS_TO_TICKS(300));
            http_server_start();
        }
    } else {
        ESP_LOGW(TAG, "STA not online yet — httpd deferred to GOT_IP/watchdog");
    }
    if (online) {
        mqtt_start();
    }

    wifi_led_refresh();

    /* Peek wake src first so cycle arm can skip MAIN/PAGE provisioning wakes. */
    if (online) {
        sleep_on_sta_online();
        if (!app_epd_panel_ok()) {
            ESP_LOGW(TAG, "panel probe unhealthy — LED stays green while STA up");
        }
    } else if (hasWifi) {
        ESP_LOGI(TAG, "Wi-Fi saved but not yet online — reconnect armed");
        /* CYCLE: bound this boot even if the AP never answers (router down,
         * wrong password) — GOT_IP later runs the normal wake path. */
        sleep_arm_offline_fallback();
    } else {
        ESP_LOGI(TAG, "hold MAIN 2.5s=SoftAP, PAGE 2.5s=BLE (IO%d/IO%d); "
                 "from deep sleep: MAIN=SoftAP PAGE=BLE",
                 BTN_MAIN_GPIO, BTN_PAGE_GPIO);
        sleep_arm_offline_fallback();
    }

    /* Deep-sleep MAIN → SoftAP, PAGE → BLE (consumes wake intent). */
    boot_apply_sleep_wake_provisioning();

    /* Rollback gate: the pending-verify image is confirmed once the panel
     * probe passed and the unit is reachable (web up or STA online). GOT_IP /
     * httpd watchdog repeat the check if it is deferred here. */
    ota_selfcheck(http_server_is_up());
    if (!http_server_is_up() && !online) {
        ESP_LOGW(TAG, "deferring OTA confirm — no httpd/STA yet");
    }

    {
        int mv = battery_read_mv();
        if (mv >= 0) {
            ESP_LOGI(TAG, "battery %d mV (~%d%%)", mv, battery_soc_from_mv(mv));
        } else {
            ESP_LOGW(TAG, "battery sense unavailable");
        }
    }
    evtlog_event("BOOT", "running wifi=%d online=%d panel=%d",
                 hasWifi ? 1 : 0, online ? 1 : 0, app_epd_panel_ok() ? 1 : 0);
    ESP_LOGI(TAG, "running");
}
