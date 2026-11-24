/**
 * @file sleep.c
 * @brief Deep-sleep scheduler + MAIN/PAGE GPIO wake (ESP32-C61 GPIO0–6).
 *
 * POWER_MODE_ALWAYS:     no auto sleep; sleep_now → until MAIN/PAGE
 * POWER_MODE_CYCLE:      after wakeSec online, sleep sleepMin
 *                        (timer + MAIN/PAGE can wake early)
 * POWER_MODE_HIBERNATE:  sleep_now / save → until MAIN/PAGE
 *
 * Wake → provisioning (edge case from sleep):
 *   MAIN  → SoftAP after boot
 *   PAGE  → BLE CT-Prov after boot
 *   TIMER → normal cycle (wake URL + online window)
 * SoftAP/BLE open always cancels an armed window. CYCLE must still bound the
 * awake time of every boot, so three paths put the window back:
 *   - a button tap re-arms on release (button.c)
 *   - sleep_on_sta_online() re-arms whenever the window is not armed
 *   - sleep_arm_offline_fallback() bounds a boot that never gets an IP
 */
#include <string.h>
#include <stdlib.h>
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
#include "http.h"
#include "app_epd.h"
#include "ota.h"
#include "evtlog.h"
#include "led.h"
#include "board_ioext.h"
#include "pindefine.h"

#define TAG "-->SLP"

static esp_timer_handle_t s_sleepTimer;
static bool s_armed;
static bool s_cycleStarted; /* wake+arm once per boot */
static bool s_entering;
static sleep_wake_src_t s_wakeSrc;
static bool s_hibernatePending; /* hibernate saved while deferred (SoftAP / no STA) */
static char s_ackCmd[16];       /* MQTT cmd to ack if this entry aborts ("" = timer) */
static bool s_inSleepNow;       /* BLE close inside sleep_now() must not re-arm */

static bool prov_window_busy(void)
{
    /* Entering counts too: cutting the radio mid-BLE-bring-up is as bad. */
    return wifi_softap_is_up() || wifi_ble_prov_busy();
}

static void ack_aborted(const char *detail)
{
    /* Follow-up only when MQTT already acked ok/sleeping for this request. */
    if (s_ackCmd[0]) {
        mqtt_publish_cmd_ack(s_ackCmd, "error", detail);
        s_ackCmd[0] = '\0';
    }
}

static int wake_window_sec(void)
{
    powerAttr_t p;
    cfg_get_power_attr(&p);
    int sec = p.wakeSec;
    if (sec < POWER_WAKE_SEC_MIN) {
        sec = POWER_WAKE_SEC_DEFAULT;
    }
    if (sec > POWER_WAKE_SEC_MAX) {
        sec = POWER_WAKE_SEC_MAX;
    }
    /* SoftAP/httpd failed → keep awake longer so MAIN/PAGE can still provision. */
    if (!http_server_is_up() && sec < POWER_WAKE_SEC_MAX) {
        sec = POWER_WAKE_SEC_MAX;
    }
    return sec;
}

void sleep_capture_wake_cause(void)
{
    s_wakeSrc = SLEEP_WAKE_NONE;
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    if (cause == ESP_SLEEP_WAKEUP_TIMER) {
        s_wakeSrc = SLEEP_WAKE_TIMER;
        ESP_LOGI(TAG, "wake cause: timer");
        return;
    }
    if (cause != ESP_SLEEP_WAKEUP_GPIO) {
        ESP_LOGI(TAG, "wake cause: %d (not gpio/timer)", (int)cause);
        return;
    }
    uint64_t st = esp_sleep_get_gpio_wakeup_status();
    const bool main = (st & (1ULL << BTN_MAIN_GPIO)) != 0;
    const bool page = (st & (1ULL << BTN_PAGE_GPIO)) != 0;
    if (main && page) {
        /* Both low: SoftAP wins (MAIN). Factory still requires awake hold. */
        s_wakeSrc = SLEEP_WAKE_MAIN;
        ESP_LOGW(TAG, "wake cause: MAIN+PAGE → SoftAP");
    } else if (page) {
        s_wakeSrc = SLEEP_WAKE_PAGE;
        ESP_LOGI(TAG, "wake cause: PAGE → BLE");
    } else if (main) {
        s_wakeSrc = SLEEP_WAKE_MAIN;
        ESP_LOGI(TAG, "wake cause: MAIN → SoftAP");
    } else {
        ESP_LOGW(TAG, "wake cause: GPIO but neither MAIN/PAGE (status=0x%llx)",
                 (unsigned long long)st);
    }
}

sleep_wake_src_t sleep_get_wake_src(void)
{
    return s_wakeSrc;
}

sleep_wake_src_t sleep_consume_wake_src(void)
{
    sleep_wake_src_t w = s_wakeSrc;
    s_wakeSrc = SLEEP_WAKE_NONE;
    return w;
}

static void sleep_enable_button_wakeup(void)
{
    /* Active-low MAIN/PAGE on GPIO2/3 — both in C61 deep-sleep wake range (0–6). */
    const uint64_t mask = (1ULL << BTN_MAIN_GPIO) | (1ULL << BTN_PAGE_GPIO);
    esp_err_t err = esp_deep_sleep_enable_gpio_wakeup(mask, ESP_GPIO_WAKEUP_GPIO_LOW);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "button gpio wakeup enable failed: %s", esp_err_to_name(err));
    }
}

static void sleep_go_task(void *arg)
{
    (void)arg;
    if (prov_window_busy()) {
        ESP_LOGW(TAG, "provisioning window up — abort deep sleep");
        evtlog_event("SLP", "abort — provisioning up");
        ack_aborted("prov_busy");
        s_entering = false;
        sleep_kick(); /* provisioning up → stays cancelled; its close re-arms */
        vTaskDelete(NULL);
        return;
    }
    /* An OTA is writing (or about to write) flash: sleeping now would cut the
     * download and leave the slot half-written. A failed attempt re-arms the
     * schedule itself (ota_restore_runtime), a successful one reboots. */
    if (ota_busy()) {
        ESP_LOGW(TAG, "OTA in progress — defer deep sleep");
        evtlog_event("SLP", "defer — ota busy");
        ack_aborted("ota_busy");
        s_entering = false;
        sleep_arm_from_config();
        vTaskDelete(NULL);
        return;
    }

    powerAttr_t p;
    cfg_get_power_attr(&p);

    bool timed = (p.mode == POWER_MODE_CYCLE && p.sleepMin > 0);
    ESP_LOGW(TAG, "entering deep sleep (%s%s)",
             power_mode_str(p.mode),
             timed ? "" : ", until MAIN/PAGE");

    /* Full E6 refresh is 20–40s. Cutting EPAD mid-DRF freezes a striped /
     * gapped frame on glass. Pause new jobs + HOLD panel lock through power-off. */
    if (!app_epd_sleep_prepare(180000)) {
        ESP_LOGW(TAG, "panel still busy — defer deep sleep");
        evtlog_event("SLP", "defer — panel busy");
        ack_aborted("panel_busy");
        s_entering = false;
        sleep_arm_from_config();
        vTaskDelete(NULL);
        return;
    }

    /* The panel wait above can run for minutes: provisioning may have come up
     * meanwhile (sleep_cancel stops the timer, not this task) — never cut it. */
    if (prov_window_busy()) {
        ESP_LOGW(TAG, "provisioning came up during panel wait — defer deep sleep");
        evtlog_event("SLP", "defer — provisioning up");
        ack_aborted("prov_busy");
        app_epd_sleep_abort();
        s_entering = false;
        sleep_kick();
        vTaskDelete(NULL);
        return;
    }

    evtlog_event("SLP", "deep sleep mode=%s timed=%d",
                 power_mode_str(p.mode), timed ? 1 : 0);

    if (mqtt_is_connected() || mqtt_is_started()) {
        if (mqtt_is_connected()) {
            mqtt_publish_telemetry();
        }
        /* Longer budget: large preview publish can hold the client mutex. */
        if (mqtt_stop_wait(10000) != 0) {
            ESP_LOGW(TAG, "mqtt_stop still busy — one more try after delay");
            vTaskDelay(pdMS_TO_TICKS(500));
            if (mqtt_stop_wait(5000) != 0) {
                ESP_LOGE(TAG, "mqtt_stop failed before sleep — powering down anyway");
                evtlog_event("SLP", "mqtt_stop busy — force wifi off");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    esp_wifi_stop();
    vTaskDelay(pdMS_TO_TICKS(100));

    /* AW9523B keeps last I2C state across ESP deep-sleep — extinguish LED
     * and drop panel rail so sleep actually saves peripheral current. */
    led_set(0, 0, 0, LED_OFF);
    (void)board_ioext_led_rgb(0, 0, 0);
    (void)board_ioext_epad_power(false);
    (void)board_ioext_vbat_dect_en(false);
    vTaskDelay(pdMS_TO_TICKS(50));

    sleep_enable_button_wakeup();
    if (timed) {
        esp_err_t err =
            esp_sleep_enable_timer_wakeup((uint64_t)p.sleepMin * 60ULL * 1000000ULL);
        if (err != ESP_OK) {
            /* The cycle lives in NVS: an abort here would reboot-loop at every
             * window end. Stay awake with the buttons live instead. */
            ESP_LOGE(TAG, "timer wake setup failed: %s — deep sleep cancelled",
                     esp_err_to_name(err));
            evtlog_event("SLP", "timer wake failed");
            app_epd_sleep_abort();
            s_entering = false;
            sleep_cancel();
            vTaskDelete(NULL);
            return;
        }
        ESP_LOGI(TAG, "timer wake in %d min + MAIN/PAGE GPIO%d/%d",
                 p.sleepMin, BTN_MAIN_GPIO, BTN_PAGE_GPIO);
    } else {
        ESP_LOGI(TAG, "MAIN/PAGE GPIO%d/%d wake only",
                 BTN_MAIN_GPIO, BTN_PAGE_GPIO);
    }
    esp_deep_sleep_start();
    /* Unreachable — if deep sleep fails, release the gate. */
    app_epd_sleep_abort();
    s_entering = false;
    vTaskDelete(NULL);
}

static void sleep_timer_cb(void *arg)
{
    (void)arg;
    if (s_entering) {
        return;
    }
    s_entering = true;
    if (xTaskCreate(sleep_go_task, "sleep_go", 4096, NULL, 5, NULL) != pdPASS) {
        s_entering = false;
        ESP_LOGE(TAG, "sleep_timer: task spawn failed — re-arm cycle");
        evtlog_event("SLP", "timer spawn failed");
        sleep_arm_from_config();
    }
}

static void sleep_schedule(void)
{
    if (!s_sleepTimer) {
        const esp_timer_create_args_t args = { .name = "sleep", .callback = sleep_timer_cb };
        ESP_ERROR_CHECK(esp_timer_create(&args, &s_sleepTimer));
    }
    int wake_s = wake_window_sec();
    esp_timer_stop(s_sleepTimer);
    ESP_ERROR_CHECK(esp_timer_start_once(s_sleepTimer, (uint64_t)wake_s * 1000000ULL));
    s_armed = true;
}

void sleep_arm_from_config(void)
{
    powerAttr_t p;
    cfg_get_power_attr(&p);
    if (p.mode == POWER_MODE_CYCLE && p.sleepMin > 0) {
        ESP_LOGI(TAG, "sleep cycle armed: %d min, wake window %d s",
                 p.sleepMin, wake_window_sec());
        sleep_schedule();
    } else {
        ESP_LOGI(TAG, "no auto sleep (mode=%s)", power_mode_str(p.mode));
        sleep_cancel();
    }
}

/* app_epd_url_async can refuse the wake render (queue full / sleep pending).
 * Never run the TLS fetch inline: callers include the 2.3KB sys_evt task. */
static void wake_url_retry_task(void *arg)
{
    char *url = (char *)arg;
    vTaskDelay(pdMS_TO_TICKS(3000));
    if (app_epd_url_async(url) != 0) {
        ESP_LOGW(TAG, "wake render skipped — render queue still busy");
    }
    free(url);
    vTaskDelete(NULL);
}

static void wake_url_retry_later(const char *url)
{
    char *copy = strdup(url);
    if (!copy ||
        xTaskCreate(wake_url_retry_task, "wake_url", 3072, copy, 4, NULL) != pdPASS) {
        free(copy);
        ESP_LOGW(TAG, "wake render dropped — render queue busy");
    }
}

void sleep_on_sta_online(void)
{
    if (prov_window_busy()) {
        ESP_LOGI(TAG, "defer sleep schedule — provisioning window up");
        return;
    }

    /* Button wake into SoftAP/BLE: stay in provisioning; do not arm yet and
     * do NOT set s_cycleStarted — the window's close re-enters here for the
     * wake URL and the first arm. */
    sleep_wake_src_t w = sleep_get_wake_src();
    if (w == SLEEP_WAKE_MAIN || w == SLEEP_WAKE_PAGE) {
        ESP_LOGI(TAG, "woke by button for provisioning — skip cycle arm");
        return;
    }

    powerAttr_t p;
    cfg_get_power_attr(&p);

    /* Hibernate saved while it could not run: going online is the promised
     * "sleep arms after home Wi-Fi is up". */
    if (s_hibernatePending && p.mode == POWER_MODE_HIBERNATE) {
        s_hibernatePending = false;
        ESP_LOGI(TAG, "deferred hibernate — entering now");
        (void)sleep_now();
        return;
    }
    if (p.mode != POWER_MODE_HIBERNATE) {
        s_hibernatePending = false;
    }

    if (p.mode != POWER_MODE_CYCLE || p.sleepMin <= 0) {
        s_cycleStarted = true;
        ESP_LOGI(TAG, "always/hibernate — no sleep cycle");
        return;
    }
    if (!s_cycleStarted) {
        s_cycleStarted = true;
        if (p.wakeUrl[0] != '\0') {
            ESP_LOGI(TAG, "wake render: %s", p.wakeUrl);
            if (app_epd_url_async(p.wakeUrl) != 0) {
                wake_url_retry_later(p.wakeUrl);
            }
        }
    }
    /* The first visit starts the cycle. Later visits (GOT_IP after a BLE or
     * SoftAP window, a link blip) must not leave a cancelled window for good. */
    if (!sleep_is_armed() && !s_entering) {
        sleep_arm_from_config();
    }
}

void sleep_arm_offline_fallback(void)
{
    if (s_entering || prov_window_busy() || sleep_is_armed()) {
        return;
    }
    sleep_wake_src_t w = sleep_get_wake_src();
    if (w == SLEEP_WAKE_MAIN || w == SLEEP_WAKE_PAGE) {
        return; /* provisioning opens next; its close handles the cycle */
    }
    powerAttr_t p;
    cfg_get_power_attr(&p);
    if (p.mode != POWER_MODE_CYCLE || p.sleepMin <= 0) {
        return;
    }
    ESP_LOGI(TAG, "cycle fallback armed (%d s) — no IP this boot yet",
             wake_window_sec());
    sleep_schedule();
}

void sleep_on_prov_closed(void)
{
    /* sleep_now() closes BLE itself: that close must not schedule a cycle or
     * start a second entry behind the one about to run. */
    if (s_inSleepNow || prov_window_busy()) {
        return; /* or the other provisioning window took over */
    }
    if (s_hibernatePending) {
        powerAttr_t p;
        cfg_get_power_attr(&p);
        s_hibernatePending = false;
        if (p.mode == POWER_MODE_HIBERNATE) {
            ESP_LOGI(TAG, "provisioning closed — deferred hibernate now");
            (void)sleep_now();
            return;
        }
    }
    if (wifi_sta_is_connected()) {
        sleep_on_sta_online();
    } else {
        sleep_arm_offline_fallback();
    }
}

void sleep_set_hibernate_pending(bool on)
{
    s_hibernatePending = on;
}

void sleep_set_ack_cmd(const char *cmd)
{
    /* Only the request that actually starts the entry owns the follow-up ack. */
    if (!s_entering && cmd) {
        strlcpy(s_ackCmd, cmd, sizeof(s_ackCmd));
    }
}

void sleep_cancel(void)
{
    if (s_sleepTimer) {
        esp_timer_stop(s_sleepTimer);
    }
    s_armed = false;
}

void sleep_kick(void)
{
    /* Keep device online while the platform is actively commanding it. */
    if (wifi_softap_is_up() || wifi_ble_prov_is_up()) {
        sleep_cancel();
        return;
    }
    powerAttr_t p;
    cfg_get_power_attr(&p);
    if (p.mode == POWER_MODE_CYCLE && p.sleepMin > 0) {
        sleep_arm_from_config();
    } else {
        sleep_cancel();
    }
}

bool sleep_now(void)
{
    /* SoftAP-first: never tear down the provisioning hotspot for sleep. */
    if (wifi_softap_is_up()) {
        ESP_LOGW(TAG, "sleep_now ignored — SoftAP provisioning is up");
        evtlog_event("SLP", "ignored — softap up");
        s_ackCmd[0] = '\0';
        return false;
    }
    if (ota_busy()) {
        ESP_LOGW(TAG, "sleep_now ignored — OTA in progress");
        evtlog_event("SLP", "ignored — ota busy");
        s_ackCmd[0] = '\0';
        return false;
    }
    if (wifi_ble_prov_is_up()) {
        ESP_LOGW(TAG, "sleep_now: closing BLE first");
        s_inSleepNow = true;
        wifi_ble_prov_close();
        s_inSleepNow = false;
    }
    sleep_cancel();
    if (s_entering) {
        ESP_LOGW(TAG, "sleep_now ignored — already entering");
        return false;
    }
    s_entering = true;
    if (xTaskCreate(sleep_go_task, "sleep_go", 4096, NULL, 5, NULL) != pdPASS) {
        s_entering = false;
        s_ackCmd[0] = '\0';
        ESP_LOGE(TAG, "sleep_now: task spawn failed");
        return false;
    }
    return true;
}

bool sleep_is_armed(void)
{
    return s_armed;
}
