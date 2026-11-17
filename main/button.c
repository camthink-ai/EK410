/**
 * @file button.c
 * @brief Front-panel keys: MAIN=GPIO2, PAGE=GPIO3 (shared rocker).
 *
 * RESET is hardware EN/RST. SoftAP / BLE work runs on dedicated tasks — the
 * button poller stack is too small for wifi_softap_open / wifi_ble_prov_open.
 *
 * MAIN hold ~2.5 s: open SoftAP (blue blink); closes BLE first if up.
 * PAGE hold ~2.5 s: open BLE CT-Prov (cyan blink); if already up → close BLE.
 * MAIN+PAGE hold 6 s: factory reset
 * PAGE click: log only (paging later)
 */
#include <stdint.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "driver/gpio.h"

#include "button.h"
#include "pindefine.h"
#include "config.h"
#include "wifi.h"
#include "led.h"

#define TAG "-->BTN"
#define HOLD_AP_MS 2500
#define HOLD_BLE_MS 2500
#define HOLD_FACTORY_MS 6000
#define POLL_MS 20

static volatile bool s_provBusy;

static void softap_task(void *arg)
{
    (void)arg;
    led_set(LEDC_NET_R, LED_BLINK);
    wifi_softap_open();

    if (wifi_softap_is_up()) {
        led_set(LEDC_NET_R, LED_BLINK);
        ESP_LOGW(TAG, "SoftAP open (idle auto-close), http://192.168.4.1/");
    } else if (cfg_softap_pending_get()) {
        /*
         * BLE SKU: MAIN → SoftAP means "exit BLE, open SoftAP on SoftAP image".
         * wifi_softap_open() arms NVS pending + SoftAP SKU reboot; SoftAP is
         * not up on this boot. That is success — never flash red (looked like
         * MAIN broken after BLE provisioning).
         */
        ESP_LOGW(TAG, "SoftAP pending after BLE exit — waiting for SoftAP SKU");
    } else {
        ESP_LOGE(TAG, "SoftAP open failed");
        wifi_led_refresh();
        led_pulse(LEDC_ERR_R, 800);
    }
    s_provBusy = false;
    vTaskDelete(NULL);
}

static void ble_task(void *arg)
{
    int mode = (int)(intptr_t)arg; /* 1=open, 0=close */
    if (mode == 0) {
        ESP_LOGI(TAG, "PAGE hold: close BLE");
        wifi_ble_prov_close();
        s_provBusy = false;
        vTaskDelete(NULL);
        return;
    }

    led_set(LEDC_BLE_R, LED_BLINK);
    wifi_ble_prov_open();
#if !CONFIG_BT_NIMBLE_ENABLED
    /*
     * SoftAP SKU: success = NVS bleboot + OTA slot switch + esp_restart().
     * is_up() stays false on this image — never treat that as failure (was
     * flashing red then STA green, looking like "PAGE = broken BLE").
     */
    if (cfg_ble_prov_boot_get()) {
        ESP_LOGW(TAG, "BLE SKU boot armed — waiting for restart");
    } else {
        ESP_LOGE(TAG, "BLE open failed (SKU missing or set_boot error)");
        wifi_led_refresh(); /* back to green/blue — brief notice only */
        led_pulse(LEDC_ERR_R, 800);
    }
    s_provBusy = false;
    vTaskDelete(NULL);
    return;
#else
    if (!wifi_ble_prov_is_up()) {
        ESP_LOGE(TAG, "BLE open failed");
        wifi_led_refresh();
        led_pulse(LEDC_ERR_R, 800);
    } else {
        led_set(LEDC_BLE_R, LED_BLINK);
        ESP_LOGW(TAG, "BLE CT-Prov open (cyan; PAGE hold again to close)");
    }
    s_provBusy = false;
    vTaskDelete(NULL);
#endif
}

static void start_softap(void)
{
    if (s_provBusy) {
        return;
    }
    s_provBusy = true;
    led_set(LEDC_NET_R, LED_BLINK);
    BaseType_t ok = xTaskCreate(softap_task, "prov_ap", 8192, NULL, 5, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "SoftAP task create failed");
        s_provBusy = false;
        wifi_led_refresh();
        led_pulse(LEDC_ERR_R, 1500);
    }
}

static void start_ble_toggle(void)
{
    if (s_provBusy) {
        return;
    }
    /* SoftAP SKU: OTA-switch to ek410_ble. BLE SKU: close → SoftAP SKU. */
    s_provBusy = true;
    int mode = wifi_ble_prov_is_up() ? 0 : 1;
    if (mode == 1) {
        led_set(LEDC_BLE_R, LED_BLINK);
    }
    BaseType_t ok = xTaskCreate(ble_task, "prov_ble", 8192, (void *)(intptr_t)mode, 5, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "BLE task create failed");
        s_provBusy = false;
        wifi_led_refresh();
        led_pulse(LEDC_ERR_R, 1500);
    }
}

static bool main_down(void)
{
    return gpio_get_level(BTN_MAIN_GPIO) == 0;
}

static bool page_down(void)
{
    return gpio_get_level(BTN_PAGE_GPIO) == 0;
}

static void button_task(void *arg)
{
    (void)arg;
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << BTN_MAIN_GPIO) | (1ULL << BTN_PAGE_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);

    ESP_LOGI(TAG, "MAIN=IO%d PAGE=IO%d (RESET=hw)", BTN_MAIN_GPIO, BTN_PAGE_GPIO);

    /* First-key-wins: rocker crosstalk often pulls the other line low briefly.
     * Old logic zeroed PAGE/MAIN timers whenever both read low → BLE never
     * armed, and a 6s both-hold looked like "PAGE = red" (factory). */
    enum { LOCK_NONE = 0, LOCK_MAIN, LOCK_PAGE, LOCK_BOTH };
    int lock = LOCK_NONE;
    int mainHeldMs = 0;
    int pageHeldMs = 0;
    int bothHeldMs = 0;
    int secondKeyMs = 0; /* how long the other key has been down while locked */
    bool apFired = false, bleFired = false, factoryFired = false;
    bool pageWasDown = false;
    bool prevMain = false, prevPage = false;
    const int BOTH_PROMOTE_MS = 400; /* ignore <400ms crosstalk on 2nd key */

    while (1) {
        bool main = main_down();
        bool page = page_down();

        if (main != prevMain || page != prevPage) {
            ESP_LOGI(TAG, "keys MAIN=%d PAGE=%d lock=%d",
                     main ? 1 : 0, page ? 1 : 0, lock);
            prevMain = main;
            prevPage = page;
        }

        if (!main && !page) {
            if (pageWasDown && pageHeldMs > 0 && pageHeldMs < HOLD_BLE_MS &&
                !bleFired && lock == LOCK_PAGE) {
                ESP_LOGI(TAG, "PAGE click");
            }
            lock = LOCK_NONE;
            mainHeldMs = pageHeldMs = bothHeldMs = secondKeyMs = 0;
            apFired = bleFired = factoryFired = false;
            pageWasDown = false;
            vTaskDelay(pdMS_TO_TICKS(POLL_MS));
            continue;
        }

        if (lock == LOCK_NONE) {
            if (main && page) {
                lock = LOCK_BOTH;
            } else if (main) {
                lock = LOCK_MAIN;
            } else {
                lock = LOCK_PAGE;
            }
            ESP_LOGI(TAG, "gesture lock=%d MAIN=%d PAGE=%d",
                     lock, main ? 1 : 0, page ? 1 : 0);
        } else if (lock == LOCK_MAIN || lock == LOCK_PAGE) {
            bool other = (lock == LOCK_MAIN) ? page : main;
            if (other) {
                secondKeyMs += POLL_MS;
                if (secondKeyMs >= BOTH_PROMOTE_MS) {
                    lock = LOCK_BOTH;
                    ESP_LOGI(TAG, "gesture → BOTH (2nd key %dms)", secondKeyMs);
                }
            } else {
                secondKeyMs = 0;
            }
        }

        if (lock == LOCK_BOTH) {
            bothHeldMs += POLL_MS;
            if (bothHeldMs >= HOLD_FACTORY_MS && !factoryFired) {
                factoryFired = true;
                ESP_LOGW(TAG, "MAIN+PAGE 6s: factory reset");
                led_set(LEDC_ERR_R, LED_BLINK_FAST);
                cfg_factory_reset();
                vTaskDelay(pdMS_TO_TICKS(400));
                esp_restart();
            }
        } else if (lock == LOCK_MAIN && main) {
            mainHeldMs += POLL_MS;
            if (mainHeldMs >= HOLD_AP_MS && !apFired) {
                apFired = true;
                ESP_LOGI(TAG, "MAIN hold %.1fs: SoftAP", HOLD_AP_MS / 1000.0f);
                start_softap();
            }
        } else if (lock == LOCK_PAGE && page) {
            pageHeldMs += POLL_MS;
            pageWasDown = true;
            if (pageHeldMs >= HOLD_BLE_MS && !bleFired) {
                bleFired = true;
                ESP_LOGI(TAG, "PAGE hold %.1fs: BLE %s",
                         HOLD_BLE_MS / 1000.0f,
                         wifi_ble_prov_is_up() ? "close" : "open");
                start_ble_toggle();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

void button_open(void)
{
    xTaskCreate(button_task, "button", 3072, NULL, 4, NULL);
    ESP_LOGI(TAG, "button ready: MAIN 2.5s=SoftAP, PAGE 2.5s=BLE toggle, MAIN+PAGE 6s=factory");
}
