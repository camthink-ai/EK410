/**
 * @file sleep.c
 * @brief Deep-sleep scheduler + MAIN GPIO wake (ESP32-C61 GPIO0–6).
 *
 * POWER_MODE_ALWAYS:     no auto sleep; sleep_now → until MAIN
 * POWER_MODE_CYCLE:      after WAKE_WINDOW_S online, sleep sleepMin minutes
 *                        (timer + MAIN can wake early)
 * POWER_MODE_HIBERNATE:  no auto cycle; sleep_now / save → until MAIN
 * SoftAP open always cancels an armed window.
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_sleep.h"
#include "esp_wifi.h"

#include "sleep.h"
#include "config.h"
#include "mqtt.h"
#include "wifi.h"
#include "app_epd.h"
#include "pindefine.h"

#define TAG "-->SLP"
#define WAKE_WINDOW_S 120

static esp_timer_handle_t s_sleepTimer;
static bool s_armed;
static bool s_cycleStarted; /* wake+arm once per boot */
static bool s_entering;

static void sleep_enable_main_wakeup(void)
{
    /* Active-low MAIN on GPIO2 — in C61 deep-sleep wake range (0–6). */
    esp_err_t err = esp_deep_sleep_enable_gpio_wakeup(
        1ULL << BTN_MAIN_GPIO, ESP_GPIO_WAKEUP_GPIO_LOW);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "MAIN gpio wakeup enable failed: %s", esp_err_to_name(err));
    }
}

static void sleep_go_task(void *arg)
{
    (void)arg;
    if (wifi_softap_is_up()) {
        ESP_LOGW(TAG, "SoftAP up — abort deep sleep");
        s_entering = false;
        vTaskDelete(NULL);
        return;
    }

    powerAttr_t p;
    cfg_get_power_attr(&p);

    bool timed = (p.mode == POWER_MODE_CYCLE && p.sleepMin > 0);
    ESP_LOGW(TAG, "entering deep sleep (%s%s)",
             power_mode_str(p.mode),
             timed ? "" : ", until MAIN");

    if (mqtt_is_connected()) {
        mqtt_publish_telemetry();
        mqtt_stop();
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    esp_wifi_stop();
    vTaskDelay(pdMS_TO_TICKS(100));

    sleep_enable_main_wakeup();
    if (timed) {
        ESP_ERROR_CHECK(esp_sleep_enable_timer_wakeup(
            (uint64_t)p.sleepMin * 60ULL * 1000000ULL));
        ESP_LOGI(TAG, "timer wake in %d min + MAIN GPIO%d", p.sleepMin, BTN_MAIN_GPIO);
    } else {
        ESP_LOGI(TAG, "MAIN GPIO%d wake only", BTN_MAIN_GPIO);
    }
    esp_deep_sleep_start();
    vTaskDelete(NULL); /* unreachable */
}

static void sleep_timer_cb(void *arg)
{
    (void)arg;
    if (s_entering) {
        return;
    }
    s_entering = true;
    xTaskCreate(sleep_go_task, "sleep_go", 4096, NULL, 5, NULL);
}

static void sleep_schedule(void)
{
    if (!s_sleepTimer) {
        const esp_timer_create_args_t args = { .name = "sleep", .callback = sleep_timer_cb };
        ESP_ERROR_CHECK(esp_timer_create(&args, &s_sleepTimer));
    }
    esp_timer_stop(s_sleepTimer);
    ESP_ERROR_CHECK(esp_timer_start_once(s_sleepTimer, (uint64_t)WAKE_WINDOW_S * 1000000ULL));
    s_armed = true;
}

void sleep_arm_from_config(void)
{
    powerAttr_t p;
    cfg_get_power_attr(&p);
    if (p.mode == POWER_MODE_CYCLE && p.sleepMin > 0) {
        ESP_LOGI(TAG, "sleep cycle armed: %d min, wake window %d s", p.sleepMin, WAKE_WINDOW_S);
        sleep_schedule();
    } else {
        ESP_LOGI(TAG, "no auto sleep (mode=%s)", power_mode_str(p.mode));
        sleep_cancel();
    }
}

void sleep_on_sta_online(void)
{
    if (s_cycleStarted) {
        return;
    }
    if (wifi_softap_is_up()) {
        ESP_LOGI(TAG, "defer sleep cycle — SoftAP up");
        return;
    }
    powerAttr_t p;
    cfg_get_power_attr(&p);
    s_cycleStarted = true;

    /* After button wake from hibernate/always sleep_now: stay awake (no re-arm). */
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    if (cause == ESP_SLEEP_WAKEUP_GPIO && p.mode != POWER_MODE_CYCLE) {
        ESP_LOGI(TAG, "woke by MAIN — stay awake (mode=%s)", power_mode_str(p.mode));
        return;
    }

    if (p.mode != POWER_MODE_CYCLE || p.sleepMin <= 0) {
        ESP_LOGI(TAG, "always/hibernate — no sleep cycle");
        return;
    }
    if (p.wakeUrl[0] != '\0') {
        ESP_LOGI(TAG, "wake render: %s", p.wakeUrl);
        if (app_epd_url_async(p.wakeUrl) != 0) {
            epd_url_frame(p.wakeUrl);
        }
    }
    sleep_arm_from_config();
}

void sleep_cancel(void)
{
    if (s_sleepTimer) {
        esp_timer_stop(s_sleepTimer);
    }
    s_armed = false;
}

void sleep_now(void)
{
    if (wifi_softap_is_up()) {
        ESP_LOGW(TAG, "sleep_now: closing SoftAP first");
        wifi_softap_close();
    }
    sleep_cancel();
    if (s_entering) {
        return;
    }
    s_entering = true;
    xTaskCreate(sleep_go_task, "sleep_go", 4096, NULL, 5, NULL);
}

bool sleep_is_armed(void)
{
    return s_armed;
}
