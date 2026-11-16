/**
 * @file button.c
 * @brief Front-panel keys: MAIN=GPIO2, PAGE=GPIO3 (shared rocker).
 *
 * RESET is hardware EN/RST. SoftAP work runs on a dedicated task — the
 * button poller stack is too small for wifi_softap_open (+ setup panel).
 *
 * MAIN hold ~2.5 s: open SoftAP (blue blink); panel guide is painted by
 * wifi_softap_open after the AP is actually up — not at device init.
 * MAIN+PAGE hold 6 s: factory reset
 * PAGE click: log only (paging later)
 */
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
#define HOLD_FACTORY_MS 6000
#define POLL_MS 20

static volatile bool s_provBusy;

static void provisioning_task(void *arg)
{
    (void)arg;
    led_set(LEDC_NET_R, LED_BLINK);
    /* Opens radio + paints SoftAP setup guide (SSID / pass / URL QR). */
    wifi_softap_open();

    if (!wifi_softap_is_up()) {
        ESP_LOGE(TAG, "SoftAP open failed");
        led_set(LEDC_ERR_R, LED_BLINK);
    } else {
        led_set(LEDC_NET_R, LED_BLINK);
        ESP_LOGW(TAG, "SoftAP open (idle auto-close), http://192.168.4.1/");
    }
    s_provBusy = false;
    vTaskDelete(NULL);
}

static void start_provisioning(void)
{
    if (s_provBusy) {
        return;
    }
    /* SoftAP already up: still spawn so guide can refresh (idempotent open). */
    s_provBusy = true;
    led_set(LEDC_NET_R, LED_BLINK);
    BaseType_t ok = xTaskCreate(provisioning_task, "prov", 8192, NULL, 5, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "prov task create failed");
        s_provBusy = false;
        led_set(LEDC_ERR_R, LED_BLINK);
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

    int mainHeldMs = 0;
    int bothHeldMs = 0;
    bool apFired = false, factoryFired = false;
    bool pageWasDown = false;

    while (1) {
        bool main = main_down();
        bool page = page_down();

        if (main && page) {
            bothHeldMs += POLL_MS;
            mainHeldMs = 0;
            if (bothHeldMs >= HOLD_FACTORY_MS && !factoryFired) {
                factoryFired = true;
                ESP_LOGW(TAG, "MAIN+PAGE 6s: factory reset");
                led_set(LEDC_ERR_R, LED_BLINK_FAST);
                cfg_factory_reset();
                vTaskDelay(pdMS_TO_TICKS(400));
                esp_restart();
            }
        } else {
            bothHeldMs = 0;
            factoryFired = false;
        }

        if (main && !page) {
            mainHeldMs += POLL_MS;
            if (mainHeldMs >= HOLD_AP_MS && !apFired) {
                apFired = true;
                ESP_LOGI(TAG, "MAIN hold %.1fs: SoftAP", HOLD_AP_MS / 1000.0f);
                start_provisioning();
            }
        } else if (!main) {
            mainHeldMs = 0;
            apFired = false;
        }

        if (page && !main) {
            if (!pageWasDown) {
                pageWasDown = true;
                ESP_LOGI(TAG, "PAGE click");
            }
        } else if (!page) {
            pageWasDown = false;
        }

        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

void button_open(void)
{
    xTaskCreate(button_task, "button", 3072, NULL, 4, NULL);
    ESP_LOGI(TAG, "button ready: MAIN 2.5s=SoftAP, short MAIN wakes deep sleep, MAIN+PAGE 6s=factory");
}
