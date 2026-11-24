/**
 * @file button.c
 * @brief Front-panel keys: MAIN=GPIO2, PAGE=GPIO3 (shared rocker).
 *
 * RESET is hardware EN/RST. SoftAP / BLE run on a statically-allocated
 * prov worker — on-demand xTaskCreate under Wi-Fi/httpd fails (red LED),
 * and a heap-backed worker fragments internal RAM until wifi aborts.
 *
 * MAIN hold ~2.5 s: open SoftAP (blue blink); closes BLE first if up.
 * PAGE hold ~2.5 s: open BLE CT-Prov (cyan blink); if already up → close BLE.
 * MAIN+PAGE hold 6 s: factory reset
 * PAGE/MAIN short press: no LED change (long-press only owns cyan/blue).
 * PAGE click: log only (paging later)
 */
#include <stdint.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_system.h"
#include "driver/gpio.h"

#include "button.h"
#include "pindefine.h"
#include "config.h"
#include "wifi.h"
#include "led.h"
#include "sleep.h"

#define TAG "-->BTN"
#define HOLD_AP_MS 2500
#define HOLD_BLE_MS 2500
#define HOLD_FACTORY_MS 6000
#define POLL_MS 20
/* Static DRAM — does not fragment the heap wifi/httpd need. */
#define PROV_WORKER_STACK 4096

enum {
    PROV_CMD_SOFTAP = 1,
    PROV_CMD_BLE_OPEN = 2,
    PROV_CMD_BLE_CLOSE = 3,
};

static QueueHandle_t s_provQ;
static StaticQueue_t s_provQmem;
static uint8_t s_provQstorage[2 * sizeof(int)];
static StackType_t s_provStack[PROV_WORKER_STACK / sizeof(StackType_t)];
static StaticTask_t s_provTcb;
static TaskHandle_t s_provTask;
static volatile bool s_provBusy;

static void prov_worker(void *arg)
{
    (void)arg;
    int cmd;
    while (1) {
        if (xQueueReceive(s_provQ, &cmd, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        s_provBusy = true;
        if (cmd == PROV_CMD_SOFTAP) {
            led_set(LEDC_NET_R, LED_BLINK);
            wifi_softap_open();
            if (wifi_softap_is_up()) {
                led_set(LEDC_NET_R, LED_BLINK);
                ESP_LOGW(TAG, "SoftAP open (idle auto-close), http://192.168.4.1/");
            } else {
                ESP_LOGE(TAG, "SoftAP open failed");
                wifi_led_refresh();
                led_pulse(LEDC_ERR_R, 800);
            }
        } else if (cmd == PROV_CMD_BLE_CLOSE) {
            ESP_LOGI(TAG, "PAGE hold: close BLE");
            wifi_ble_prov_close();
        } else if (cmd == PROV_CMD_BLE_OPEN) {
            led_set(LEDC_BLE_R, LED_BLINK);
            wifi_ble_prov_open();
            if (!wifi_ble_prov_is_up()) {
                ESP_LOGE(TAG, "BLE open failed");
                wifi_led_refresh();
                led_pulse(LEDC_ERR_R, 800);
            } else {
                led_set(LEDC_BLE_R, LED_BLINK);
                ESP_LOGW(TAG, "BLE CT-Prov open (cyan; PAGE hold again to close)");
            }
        }
        s_provBusy = false;
    }
}

static bool post_prov(int cmd)
{
    if (!s_provQ || !s_provTask) {
        ESP_LOGE(TAG, "prov worker not ready");
        return false;
    }
    if (s_provBusy) {
        ESP_LOGW(TAG, "prov busy — ignore cmd %d", cmd);
        return false;
    }
    if (xQueueSend(s_provQ, &cmd, 0) != pdTRUE) {
        ESP_LOGE(TAG, "prov queue full (cmd %d)", cmd);
        return false;
    }
    return true;
}

static void start_softap(void)
{
    led_set(LEDC_NET_R, LED_BLINK);
    if (!post_prov(PROV_CMD_SOFTAP)) {
        wifi_led_refresh();
        led_pulse(LEDC_ERR_R, 800);
    }
}

static void start_ble_toggle(void)
{
    int cmd = wifi_ble_prov_is_up() ? PROV_CMD_BLE_CLOSE : PROV_CMD_BLE_OPEN;
    if (cmd == PROV_CMD_BLE_OPEN) {
        led_set(LEDC_BLE_R, LED_BLINK);
    }
    if (!post_prov(cmd)) {
        wifi_led_refresh();
        led_pulse(LEDC_ERR_R, 800);
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

    ESP_LOGI(TAG, "MAIN=IO%d PAGE=IO%d (RESET=hw) levels MAIN=%d PAGE=%d",
             BTN_MAIN_GPIO, BTN_PAGE_GPIO,
             main_down() ? 1 : 0, page_down() ? 1 : 0);

    /* First-key-wins: rocker crosstalk often pulls the other line low briefly.
     * Old logic zeroed PAGE/MAIN timers whenever both read low → BLE never
     * armed, and a 6s both-hold looked like "PAGE = red" (factory). */
    enum { LOCK_NONE = 0, LOCK_MAIN, LOCK_PAGE, LOCK_BOTH };
    int lock = LOCK_NONE;
    int mainHeldMs = 0;
    int pageHeldMs = 0;
    int bothHeldMs = 0;
    int secondKeyMs = 0;
    bool apFired = false, bleFired = false, factoryFired = false;
    bool pageWasDown = false;
    bool prevMain = false, prevPage = false;
    bool mainStable = false, pageStable = false;
    bool mainCand = false, pageCand = false;
    int mainStabMs = 0, pageStabMs = 0;
    const int DEBOUNCE_MS = 60;
    const int BOTH_PROMOTE_MS = 400;
    int sampleMs = 0;

    while (1) {
        bool mainRaw = main_down();
        bool pageRaw = page_down();

        /* 60ms debounce — rocker bounce was flashing cyan↔green↔blue. */
        if (mainRaw != mainCand) {
            mainCand = mainRaw;
            mainStabMs = 0;
        } else if (mainStabMs < DEBOUNCE_MS) {
            mainStabMs += POLL_MS;
            if (mainStabMs >= DEBOUNCE_MS) {
                mainStable = mainCand;
            }
        }
        if (pageRaw != pageCand) {
            pageCand = pageRaw;
            pageStabMs = 0;
        } else if (pageStabMs < DEBOUNCE_MS) {
            pageStabMs += POLL_MS;
            if (pageStabMs >= DEBOUNCE_MS) {
                pageStable = pageCand;
            }
        }
        bool main = mainStable;
        bool page = pageStable;

        /* 1 Hz raw sample for first 90s — proves GPIO wiring while debugging.
         * MAIN/PAGE here are pressed=1; t= is elapsed sample time, not hold. */
        sampleMs += POLL_MS;
        if (sampleMs <= 90000 && (sampleMs % 1000) == 0) {
            ESP_LOGI(TAG, "sample MAIN=%d PAGE=%d (t=%ds)",
                     main ? 1 : 0, page ? 1 : 0, sampleMs / 1000);
        }

        if (main != prevMain || page != prevPage) {
            ESP_LOGI(TAG, "keys MAIN=%d PAGE=%d lock=%d",
                     main ? 1 : 0, page ? 1 : 0, lock);
            if (main || page) {
                sleep_cancel();
            }
            prevMain = main;
            prevPage = page;
        }

        if (!main && !page) {
            if (pageWasDown && pageHeldMs > 0 && pageHeldMs < HOLD_BLE_MS &&
                !bleFired && lock == LOCK_PAGE) {
                ESP_LOGI(TAG, "PAGE click");
            }
            /* Only restore idle LED on short press; long-press owns cyan/blue. */
            if (lock != LOCK_NONE && !apFired && !bleFired && !factoryFired) {
                wifi_led_refresh();
                /* The press above cancelled the CYCLE window: a tap that fired
                 * no gesture must put it back, or the unit never sleeps again. */
                sleep_kick();
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

        /* LED only on long-press action — short PAGE/MAIN must not flash cyan/blue. */
        if (lock == LOCK_BOTH) {
            /* LOCK_BOTH latches on a ≥400 ms second key: count only while BOTH
             * keys are still down, so a brushed PAGE tap cannot arm a factory
             * reset that completes on MAIN alone. */
            if (main && page) {
                bothHeldMs += POLL_MS;
            } else {
                bothHeldMs = 0;
            }
            if (bothHeldMs >= HOLD_FACTORY_MS && !factoryFired) {
                factoryFired = true;
                ESP_LOGW(TAG, "MAIN+PAGE 6s: factory reset");
                led_set(LEDC_ERR_R, LED_BLINK_FAST);
                if (cfg_factory_reset() != ESP_OK) {
                    ESP_LOGE(TAG, "factory reset NVS failed — restarting anyway");
                }
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
    s_provQ = xQueueCreateStatic(2, sizeof(int), s_provQstorage, &s_provQmem);
    s_provTask = xTaskCreateStatic(prov_worker, "prov_w", PROV_WORKER_STACK,
                                   NULL, 5, s_provStack, &s_provTcb);
    if (!s_provQ || !s_provTask) {
        ESP_LOGE(TAG, "static prov worker/queue failed");
    } else {
        ESP_LOGI(TAG, "prov worker ready (static stack=%u)",
                 (unsigned)PROV_WORKER_STACK);
    }
    xTaskCreate(button_task, "button", 3072, NULL, 4, NULL);
    ESP_LOGI(TAG, "button ready: MAIN 2.5s=SoftAP, PAGE 2.5s=BLE toggle, MAIN+PAGE 6s=factory");
}
