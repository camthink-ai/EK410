/**
 * @file wifi.c
 * @brief STA with try-then-persist + exponential reconnect; SoftAP + BLE windows.
 *
 * SoftAP (MAIN hold) and BLE CT-Prov (PAGE hold) are mutually exclusive.
 * SoftAP stays up while HTTP / SoftAP clients are active; BLE stays up while
 * a GATT client is linked or recent GATT traffic (wifi_ble_prov_touch).
 *
 * Credentials are applied via wifi_sta_try_connect(); NVS write is the
 * caller's job on success. STA disconnect uses backoff reconnect (1s→60s).
 */
#include <stdio.h>
#include <string.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#if CONFIG_BT_NIMBLE_ENABLED
#include "esp_coexist.h"
#endif

#include "wifi.h"
#include "ct_prov.h"
#include "ct_prov_ble.h"
#include "http.h"
#include "config.h"
#include "mqtt.h"
#include "led.h"
#include "sleep.h"
#include "app_epd.h"
#include "evtlog.h"
#include "ota.h"

#define TAG "-->WIFI"
#define AP_PASS "12345678"
#define STA_CONNECT_TIMEOUT_MS 20000
#define SOFTAP_IDLE_MS         (2 * 60 * 1000)
#define BLE_PROV_IDLE_MS       (2 * 60 * 1000) /* also PAGE / session.close / SoftAP */
/* SoftAP open stops MQTT then must bind httpd before mqtt_start (which pauses
 * httpd). Without this window phones join SoftAP into a dark 192.168.4.1. */
#define SOFTAP_MQTT_DEFER_US   (2000000ULL) /* SoftAP web bind, then restore MQTT */
#define SOFTAP_OPEN_KEEP_MQTT_INT 24576     /* skip mqtt_stop when largest_int already OK */
#define RECONNECT_MIN_MS       1000
#define RECONNECT_MAX_MS       60000
/* First STA join on some 11ax APs fails once with ASSOC_FAIL (203). During
 * wifi_sta_start() s_userConnectInFlight used to suppress reconnect_arm(), so
 * we waited the full 20s timeout before a second attempt. Quick in-flight
 * retries cut that to a few hundred ms. */
#define INFLIGHT_ASSOC_RETRY_MS   300
#define INFLIGHT_ASSOC_RETRY_MAX  3
#define STA_CONNECT_SETTLE_MS     250

static EventGroupHandle_t s_wifiEvent;
static esp_netif_t *s_staNetif;
static esp_netif_t *s_apNetif;
static esp_timer_handle_t s_apIdleTimer;
static esp_timer_handle_t s_bleIdleTimer;
static esp_timer_handle_t s_reconnectTimer;
static esp_timer_handle_t s_httpdRetryTimer;
static esp_timer_handle_t s_httpdWatchdogTimer;
static esp_timer_handle_t s_softapMqttDeferTimer;
static volatile bool s_softapMqttDeferPending;
static int s_httpdRetryLeft;
static bool s_staStarted;
static bool s_wifiStarted;
static bool s_wifiDriverUp; /* esp_wifi_init done */
/* C61 internal RAM is tight with httpd+MQTT. Default lean buffers on cold
 * boot too — full defaults left largest_int ≈4KB after GOT_IP and MQTT
 * start aborted in phy_track_pll_init (ESP_ERR_NO_MEM). After NimBLE
 * teardown this flag stays true for the same reason. */
static bool s_wifiLeanBuffers = true;
static char s_lastProvErr[96];

const char *wifi_last_error(void)
{
    return s_lastProvErr;
}

void wifi_clear_last_error(void)
{
    s_lastProvErr[0] = '\0';
}

static void wifi_set_last_error(const char *msg)
{
    if (!msg || !msg[0]) {
        s_lastProvErr[0] = '\0';
        return;
    }
    strlcpy(s_lastProvErr, msg, sizeof(s_lastProvErr));
}
static bool s_softapUp;
static bool s_softapOpening; /* STA→APSTA in progress — suppress reconnect thrash */
static bool s_softapClosing; /* suppress LED thrash while APSTA→STA tears down */
static bool s_softapPaintedGuide; /* restore CamThink logo when SoftAP closes */
static bool s_bleProvUp;
static bool s_bleEntering; /* teardown→NimBLE in progress — lock LED to cyan */
static bool s_bleProvClosing;
static int s_apClients;
static wifi_sta_state_t s_staState;
static int s_reconnectDelayMs = RECONNECT_MIN_MS;
static bool s_userConnectInFlight; /* suppress auto-reconnect during try */
static bool s_inflightQuickRetry;  /* allow reconnect_timer during try */
static int s_inflightRetryCount;

static bool sta_reason_inflight_retryable(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_ASSOC_FAIL:
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_NO_AP_FOUND:
        return true;
    default:
        return false;
    }
}

static void inflight_assoc_retry_arm(uint8_t reason)
{
    if (!s_reconnectTimer || !s_staStarted || !s_userConnectInFlight) {
        return;
    }
    if (s_inflightRetryCount >= INFLIGHT_ASSOC_RETRY_MAX) {
        ESP_LOGW(TAG, "in-flight assoc retries exhausted (reason=%u)",
                 (unsigned)reason);
        return;
    }
    s_inflightRetryCount++;
    s_inflightQuickRetry = true;
    esp_timer_stop(s_reconnectTimer);
    esp_timer_start_once(s_reconnectTimer,
                         (uint64_t)INFLIGHT_ASSOC_RETRY_MS * 1000ULL);
    ESP_LOGI(TAG, "in-flight assoc retry #%d in %d ms (reason=%u)",
             s_inflightRetryCount, INFLIGHT_ASSOC_RETRY_MS, (unsigned)reason);
}

static void httpd_arm_retry(int left);
static void httpd_wait_heap_for_start(void);
static void wifi_led_apply_idle_state(void);

static void softap_mqtt_defer_cb(void *arg)
{
    (void)arg;
    s_softapMqttDeferPending = false;
    if (s_bleProvUp || s_bleEntering || wifi_ble_prov_is_up()) {
        return;
    }
    if (!s_softapUp) {
        return;
    }
    if (!wifi_sta_is_connected()) {
        ESP_LOGI(TAG, "SoftAP MQTT defer — STA offline, skip");
        return;
    }
    if (!http_server_is_up()) {
        httpd_wait_heap_for_start();
        http_server_start();
        if (!http_server_is_up()) {
            ESP_LOGW(TAG, "SoftAP MQTT defer — httpd still down");
            httpd_arm_retry(20);
            return;
        }
    }
    ESP_LOGI(TAG, "SoftAP MQTT defer fired — start MQTT (web already bound)");
    mqtt_start();
}

static void softap_arm_mqtt_defer(void)
{
    if (!s_softapMqttDeferTimer) {
        return;
    }
    s_softapMqttDeferPending = true;
    esp_timer_stop(s_softapMqttDeferTimer);
    esp_timer_start_once(s_softapMqttDeferTimer, SOFTAP_MQTT_DEFER_US);
    ESP_LOGI(TAG, "SoftAP — MQTT deferred %u ms so SoftAP web can bind",
             (unsigned)(SOFTAP_MQTT_DEFER_US / 1000ULL));
}

static void softap_cancel_mqtt_defer(void)
{
    s_softapMqttDeferPending = false;
    if (s_softapMqttDeferTimer) {
        esp_timer_stop(s_softapMqttDeferTimer);
    }
}

void wifi_softap_mqtt_user_start(void)
{
    softap_cancel_mqtt_defer();
}

bool wifi_softap_mqtt_defer_pending(void)
{
    return s_softapMqttDeferPending && s_softapUp;
}

static void httpd_arm_retry(int left)
{
    if (!s_httpdRetryTimer) {
        return;
    }
    s_httpdRetryLeft = left > 0 ? left : 1;
    esp_timer_stop(s_httpdRetryTimer);
    esp_timer_start_once(s_httpdRetryTimer, 500000ULL);
}

static void httpd_wait_heap_for_start(void)
{
    for (int i = 0; i < 40; i++) {
        size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
        if (largest >= 7168) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static bool s_httpdPausedForMqtt;

int wifi_httpd_pause_for_mqtt(void)
{
    if (s_bleProvUp || s_bleEntering || wifi_ble_prov_is_up()) {
        return 0;
    }
    /*
     * Claim the SoftAP MQTT window even when httpd is already down — otherwise
     * watchdog/retry can start the web UI while MQTT waits for internal heap.
     */
    if (!http_server_is_up() && !http_server_is_stopping()) {
        s_httpdPausedForMqtt = true;
        ESP_LOGI(TAG, "httpd already down — hold pause for MQTT (softap=%d largest_int=%u)",
                 s_softapUp ? 1 : 0,
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        return 1;
    }
    ESP_LOGI(TAG, "pause httpd for MQTT (softap=%d largest_int=%u)",
             s_softapUp ? 1 : 0,
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    /* Mark BEFORE arming stop so retry/watchdog cannot restart mid-pause. */
    s_httpdPausedForMqtt = true;
    http_server_stop();
    if (http_server_stop_wait(2000) != 0 || http_server_is_stopping() ||
        http_server_is_up()) {
        /* One more beat — SoftAP clients / lagging workers often release then. */
        vTaskDelay(pdMS_TO_TICKS(200));
        if (http_server_is_up() || http_server_is_stopping()) {
            http_server_stop();
        }
        if (http_server_stop_wait(2500) != 0 || http_server_is_stopping() ||
            http_server_is_up()) {
            /*
             * Keep the pause claim so watchdog cannot bounce httpd while MQTT
             * retries. Clearing the flag raced SoftAP UI back into the heap.
             */
            ESP_LOGW(TAG, "httpd pause incomplete — keep pause claim for retry");
            return -1;
        }
    }
    vTaskDelay(pdMS_TO_TICKS(150));
    ESP_LOGI(TAG, "httpd paused largest_int=%u",
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    return 1;
}

void wifi_httpd_resume_after_mqtt(void)
{
    if (s_bleProvUp || s_bleEntering || wifi_ble_prov_is_up()) {
        s_httpdPausedForMqtt = false;
        return;
    }
    if (!s_httpdPausedForMqtt && http_server_is_up()) {
        wifi_led_apply_idle_state();
        return;
    }
    s_httpdPausedForMqtt = false;
    if (http_server_is_up()) {
        wifi_led_apply_idle_state();
        return;
    }
    httpd_wait_heap_for_start();
    ESP_LOGI(TAG, "resume httpd after MQTT (softap=%d largest_int=%u)",
             s_softapUp ? 1 : 0,
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    http_server_start();
    if (!http_server_is_up()) {
        /* Pause claim already cleared — let retry/watchdog bring LAN web back. */
        ESP_LOGW(TAG, "resume httpd failed — arm retry (LAN must recover)");
        httpd_arm_retry(30);
    }
    /* Pause/resume must not leave the status LED stuck off. */
    wifi_led_apply_idle_state();
}

void wifi_coalesce_for_mqtt(void)
{
    if (wifi_httpd_pause_for_mqtt() == 1) {
        vTaskDelay(pdMS_TO_TICKS(50));
        wifi_httpd_resume_after_mqtt();
    }
}

void wifi_network_services_restore(bool want_mqtt)
{
    if (s_bleProvUp || s_bleEntering || wifi_ble_prov_is_up()) {
        ESP_LOGW(TAG, "services restore skip — BLE owns radio");
        return;
    }
    /* Caller may have armed async stop (SoftAP close); wait it out. Do not
     * bounce a healthy httpd — that races SoftAP UI and BLE exit. */
    if (http_server_is_stopping()) {
        (void)http_server_stop_wait(2000);
    }
    httpd_wait_heap_for_start();
    ESP_LOGI(TAG, "services restore heap free %u int %u largest_int %u",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

    if (!http_server_is_up()) {
        size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
        if (largest >= 7168) {
            http_server_start();
        } else {
            ESP_LOGW(TAG, "services restore httpd deferred (largest_int=%u)",
                     (unsigned)largest);
        }
    }
    if (!http_server_is_up()) {
        httpd_arm_retry(30);
        ESP_LOGW(TAG, "services restore — httpd not up, retry armed");
    }

    /* SoftAP+STA: mqtt_bringup pauses httpd briefly then resumes lean web. */
    if (want_mqtt && wifi_sta_is_connected()) {
        mqtt_start();
    }
}

/**
 * Single LED policy for "steady" state.
 * SoftAP/BLE/OTA failure paths must call this after a fault pulse — otherwise
 * red sticks when STA is already up (no second GOT_IP).
 *
 * Do NOT map panel_ok→red here: a flaky panel probe must not stick red after
 * a normal BLE idle exit.
 */
static void wifi_led_apply_idle_state(void)
{
    if (s_bleProvUp || s_bleEntering) {
        ESP_LOGI(TAG, "LED → cyan blink (BLE prov)");
        led_set(LEDC_BLE_R, LED_BLINK);
        return;
    }
    if (s_softapUp || s_softapOpening) {
        ESP_LOGI(TAG, "LED → blue blink (SoftAP)");
        led_set(LEDC_NET_R, LED_BLINK);
        return;
    }
    if (wifi_sta_is_connected()) {
        ESP_LOGI(TAG, "LED → green solid (STA up)");
        led_set(LEDC_OK_R, LED_ON);
        return;
    }
    /* Still have a home network to chase — orange, not SoftAP blue. */
    if (s_staStarted || config_has_wifi()) {
        ESP_LOGI(TAG, "LED → orange blink (joining/reconnect)");
        led_set(LEDC_JOIN_R, LED_BLINK);
        return;
    }
    ESP_LOGI(TAG, "LED → green solid (unprovisioned idle)");
    led_set(LEDC_OK_R, LED_ON); /* unprovisioned resting */
}

void wifi_led_refresh(void)
{
    wifi_led_apply_idle_state();
}

static int s_throughputBoostDepth;

void wifi_throughput_boost(bool enable)
{
    if (enable) {
        if (s_throughputBoostDepth++ == 0) {
            esp_err_t e = esp_wifi_set_ps(WIFI_PS_NONE);
            ESP_LOGI(TAG, "throughput boost ON (ps=NONE) err=%s", esp_err_to_name(e));
        }
    } else if (s_throughputBoostDepth > 0) {
        if (--s_throughputBoostDepth == 0) {
            esp_err_t e = esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
            ESP_LOGI(TAG, "throughput boost OFF (ps=MIN_MODEM) err=%s", esp_err_to_name(e));
        }
    }
}

static void softap_arm_idle(void);
static void softap_close_internal(void);
static void ble_prov_arm_idle(void);
static void reconnect_arm(void);
static bool sta_apply_and_wait(const char *ssid, const char *password, int timeout_ms);

static void softap_arm_idle(void)
{
    if (!s_apIdleTimer || !s_softapUp) {
        return;
    }
    esp_timer_stop(s_apIdleTimer);
    esp_timer_start_once(s_apIdleTimer, (uint64_t)SOFTAP_IDLE_MS * 1000ULL);
}

static void ap_idle_timer_cb(void *arg)
{
    (void)arg;
    if (!s_softapUp) {
        return;
    }
    if (s_apClients > 0) {
        ESP_LOGI(TAG, "SoftAP idle tick — %d client(s) still joined, extending", s_apClients);
        softap_arm_idle();
        return;
    }
    ESP_LOGI(TAG, "SoftAP idle %d ms — closing (clients=%d)", SOFTAP_IDLE_MS, s_apClients);
    softap_close_internal();
}

static uint32_t ble_idle_ms(void)
{
    int sec = cfg_ble_idle_sec_get();
    if (sec > 0) {
        return (uint32_t)sec * 1000u;
    }
    return (uint32_t)BLE_PROV_IDLE_MS;
}

static void ble_prov_arm_idle(void)
{
    if (!s_bleIdleTimer || !s_bleProvUp) {
        return;
    }
    esp_timer_stop(s_bleIdleTimer);
    esp_timer_start_once(s_bleIdleTimer, (uint64_t)ble_idle_ms() * 1000ULL);
}

static void ble_idle_timer_cb(void *arg)
{
    (void)arg;
    if (!s_bleProvUp) {
        return;
    }
    if (ct_prov_ble_client_connected()) {
        ESP_LOGI(TAG, "BLE idle tick — GATT client linked, extending");
        ble_prov_arm_idle();
        return;
    }
    ESP_LOGI(TAG, "BLE idle %u ms — stop NimBLE (not a fault)",
             (unsigned)ble_idle_ms());
    /* LED comes from wifi_ble_prov_close → green / blue-joining — not sticky cyan. */
    wifi_ble_prov_close();
}

static void reconnect_timer_cb(void *arg)
{
    (void)arg;
    if (!s_staStarted) {
        return;
    }
    /* Normal backoff is suppressed while try_connect waits; the in-flight
     * quick-retry path sets s_inflightQuickRetry for ASSOC_FAIL etc. */
    if (s_userConnectInFlight && !s_inflightQuickRetry) {
        return;
    }
    const bool inflight = s_inflightQuickRetry;
    s_inflightQuickRetry = false;
#if CONFIG_BT_NIMBLE_ENABLED
    /* BLE CT-Prov owns the radio — do not steal airtime for STA join. */
    if (s_bleProvUp || s_bleEntering || ct_prov_ble_is_up()) {
        return;
    }
#endif
    if (wifi_sta_is_connected()) {
        return;
    }
    if (inflight) {
        ESP_LOGI(TAG, "in-flight reconnect attempt");
    } else {
        ESP_LOGI(TAG, "reconnect attempt (backoff %d ms)", s_reconnectDelayMs);
        s_staState = WIFI_STA_RECONNECTING;
    }
    esp_wifi_connect();
}

static void reconnect_arm(void)
{
    if (!s_reconnectTimer || !s_staStarted || s_userConnectInFlight) {
        return;
    }
#if CONFIG_BT_NIMBLE_ENABLED
    if (s_bleProvUp || s_bleEntering || ct_prov_ble_is_up()) {
        return;
    }
#endif
    esp_timer_stop(s_reconnectTimer);
    esp_timer_start_once(s_reconnectTimer, (uint64_t)s_reconnectDelayMs * 1000ULL);
    int next = s_reconnectDelayMs * 2;
    if (next > RECONNECT_MAX_MS) {
        next = RECONNECT_MAX_MS;
    }
    s_reconnectDelayMs = next;
}

void wifi_softap_touch(void)
{
    if (s_softapUp) {
        softap_arm_idle();
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (!s_staStarted) {
            return;
        }
        wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)data;
        ESP_LOGW(TAG, "sta disconnected reason=%d", disc ? disc->reason : -1);
        evtlog_event("WIFI", "sta disconnect reason=%d", disc ? disc->reason : -1);
        /* Do not set FAIL_BIT here — association often disconnects once
         * before GOT_IP; try_connect waits on CONNECTED + timeout only. */
        xEventGroupClearBits(s_wifiEvent, WIFI_STA_CONNECTED_BIT);
        if (s_userConnectInFlight) {
            uint8_t reason = disc ? disc->reason : 0;
            if (sta_reason_inflight_retryable(reason)) {
                inflight_assoc_retry_arm(reason);
            }
        } else {
            s_staState = WIFI_STA_RECONNECTING;
            /* SoftAP open/close itself owns LED + reconnect; ignore APSTA churn.
             * BLE window owns cyan — do not flash blue over it. */
            if (!s_softapUp && !s_softapOpening && !s_softapClosing &&
                !s_bleProvUp && !s_bleEntering) {
                led_set(LEDC_JOIN_R, LED_BLINK);
                reconnect_arm();
            } else if (s_bleProvUp || s_bleEntering) {
                led_set(LEDC_BLE_R, LED_BLINK);
            }
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        s_apClients++;
        ESP_LOGI(TAG, "SoftAP client +1 (now %d)", s_apClients);
        softap_arm_idle();
        led_set(LEDC_NET_R, LED_BLINK);
        /* Phone joined SoftAP — ensure web is up; restore MQTT after bind. */
        if (!http_server_is_up()) {
            http_server_start();
            if (!http_server_is_up()) {
                httpd_arm_retry(20);
            }
        }
        if (wifi_sta_is_connected() && !mqtt_is_started()) {
            softap_arm_mqtt_defer();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        if (s_apClients > 0) {
            s_apClients--;
        }
        ESP_LOGI(TAG, "SoftAP client -1 (now %d)", s_apClients);
        softap_arm_idle();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_START) {
        ESP_LOGI(TAG, "WIFI_EVENT_AP_START");
        if (s_softapUp) {
            led_set(LEDC_NET_R, LED_BLINK);
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        xEventGroupSetBits(s_wifiEvent, WIFI_SCAN_DONE_BIT);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&e->ip_info.ip));
        evtlog_event("WIFI", "got_ip " IPSTR "%s", IP2STR(&e->ip_info.ip),
                     s_bleProvUp ? " (BLE window)" : "");
        evtlog_time_start();
        xEventGroupClearBits(s_wifiEvent, WIFI_STA_FAIL_BIT);
        xEventGroupSetBits(s_wifiEvent, WIFI_STA_CONNECTED_BIT);
        s_staState = WIFI_STA_CONNECTED;
        s_reconnectDelayMs = RECONNECT_MIN_MS;
        s_inflightRetryCount = 0;
        s_inflightQuickRetry = false;
        if (s_reconnectTimer) {
            esp_timer_stop(s_reconnectTimer);
        }
        if (s_bleProvUp) {
            led_set(LEDC_BLE_R, LED_BLINK);
            /* MQTT/httpd stay down for the whole BLE provisioning window. */
        } else {
            wifi_led_apply_idle_state();
            /*
             * Always httpd before mqtt on GOT_IP. mqtt has its own start-retry
             * timer; httpd used to lose SoftAP-close / cold-boot races and leave
             * LAN Web dead while the cloud still looked online.
             */
            if (!http_server_is_up()) {
                size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
                if (largest >= 7168) {
                    http_server_start();
                } else {
                    ESP_LOGW(TAG, "GOT_IP httpd deferred (largest_int=%u)",
                             (unsigned)largest);
                }
            }
            if (http_server_is_up()) {
                /* Confirm OTA once LAN web is up (covers headless / late IP). */
                ota_confirm_running();
            }
            if (s_softapUp) {
                /* SoftAP web first — mqtt_start pauses httpd; defer restore. */
                if (!http_server_is_up()) {
                    httpd_arm_retry(20);
                }
                softap_arm_mqtt_defer();
            } else if (http_server_is_up()) {
                mqtt_start();
            } else {
                ESP_LOGW(TAG, "httpd not up after GOT_IP — retry, defer mqtt");
                httpd_arm_retry(20);
            }
        }
        sleep_on_sta_online();
    }
}

static void httpd_retry_timer_cb(void *arg)
{
    (void)arg;
    if (s_bleProvUp || s_bleEntering || wifi_ble_prov_is_up()) {
        return;
    }
    if (s_httpdPausedForMqtt) {
        /* MQTT owns the pause window — do not fight for port 80. */
        if (s_httpdRetryTimer && s_httpdRetryLeft > 0) {
            esp_timer_start_once(s_httpdRetryTimer, 500000ULL);
        }
        return;
    }
    if (http_server_is_stopping()) {
        if (s_httpdRetryTimer) {
            esp_timer_start_once(s_httpdRetryTimer, 300000ULL);
        }
        return;
    }
    if (!http_server_is_up()) {
        size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
        if (largest < 7168) {
            ESP_LOGW(TAG, "httpd retry wait coalesce largest_int=%u left=%d",
                     (unsigned)largest, s_httpdRetryLeft);
        } else if (!s_httpdPausedForMqtt &&
                   !s_bleProvUp && !s_bleEntering && !wifi_ble_prov_is_up()) {
            http_server_start();
        }
    }
    if (http_server_is_up()) {
        ota_confirm_running();
        if (wifi_sta_is_connected()) {
            if (s_softapUp) {
                softap_arm_mqtt_defer();
            } else {
                mqtt_start();
            }
        }
        return;
    }
    if (s_httpdRetryLeft > 0 && s_httpdRetryTimer) {
        s_httpdRetryLeft--;
        esp_timer_start_once(s_httpdRetryTimer, 500000ULL);
    } else {
        /* Do not abandon forever — watchdog will re-arm. */
        ESP_LOGE(TAG, "httpd retry burst exhausted — watchdog will keep trying");
    }
}

static void httpd_watchdog_timer_cb(void *arg)
{
    (void)arg;
    if (s_bleProvUp || s_bleEntering || wifi_ble_prov_is_up()) {
        return;
    }
    if (s_softapOpening || s_softapClosing) {
        return;
    }
    if (s_httpdPausedForMqtt) {
        return;
    }
    /* SoftAP UI and STA LAN Web both need port 80. */
    if (!s_softapUp && !wifi_sta_is_connected()) {
        return;
    }
    if (http_server_is_up() || http_server_is_stopping()) {
        return;
    }
    ESP_LOGW(TAG, "httpd watchdog — port 80 down (softap=%d sta=%d)",
             s_softapUp ? 1 : 0, wifi_sta_is_connected() ? 1 : 0);
    evtlog_event("HTTP", "watchdog restart softap=%d sta=%d",
                 s_softapUp ? 1 : 0, wifi_sta_is_connected() ? 1 : 0);
    httpd_wait_heap_for_start();
    /* Re-check after heap wait — MQTT pause / BLE may have claimed the window. */
    if (s_bleProvUp || s_bleEntering || wifi_ble_prov_is_up() ||
        s_httpdPausedForMqtt || http_server_is_up() || http_server_is_stopping()) {
        return;
    }
    http_server_start();
    if (!http_server_is_up()) {
        httpd_arm_retry(20);
    } else {
        ota_confirm_running();
        if (wifi_sta_is_connected() && !mqtt_is_started()) {
            if (s_softapUp) {
                softap_arm_mqtt_defer();
            } else {
                mqtt_start();
            }
        }
    }
}

static esp_err_t wifi_driver_ensure(void)
{
    if (s_wifiDriverUp) {
        return ESP_OK;
    }
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    const bool lean = s_wifiLeanBuffers || s_bleProvUp || ct_prov_ble_is_up();
    /* After NimBLE re-init: trim static/dynamic buffers only. Do NOT disable
     * AMPDU/NVS or shrink wifi task stack — that caused STA reason=1 flaps
     * (assoc→run→init) before GOT_IP on Camthink_hi 11ax. */
    if (lean) {
        cfg.static_rx_buf_num = 3;
        cfg.dynamic_rx_buf_num = 6;
        cfg.tx_buf_type = 1;
        cfg.static_tx_buf_num = 0;
        cfg.dynamic_tx_buf_num = 6;
        cfg.cache_tx_buf_num = 4;
        cfg.rx_mgmt_buf_num = 4;
        cfg.mgmt_sbuf_num = 6;
        cfg.csi_enable = 0;
    }
    ESP_LOGI(TAG, "esp_wifi_init trying%s — free %u int %u largest_int %u",
             lean ? " (lean)" : "",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    esp_err_t err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init: %s (int %u largest_int %u)",
                 esp_err_to_name(err),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        return err;
    }
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    s_wifiDriverUp = true;
    ESP_LOGI(TAG, "Wi-Fi driver up — free %u int %u largest_int %u",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    return ESP_OK;
}

/** Full Wi-Fi teardown so NimBLE can claim contiguous internal RAM. */
static void wifi_driver_teardown(void)
{
    if (s_reconnectTimer) {
        esp_timer_stop(s_reconnectTimer);
    }
    if (!s_wifiDriverUp) {
        return;
    }
    if (s_wifiStarted) {
        esp_wifi_stop();
        s_wifiStarted = false;
        vTaskDelay(pdMS_TO_TICKS(120));
    }
    esp_err_t err = esp_wifi_deinit();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_deinit: %s", esp_err_to_name(err));
    }
    s_wifiDriverUp = false;
    s_wifiLeanBuffers = true; /* next esp_wifi_init must stay lean for httpd */
    s_staStarted = false;
    s_staState = WIFI_STA_IDLE;
    if (s_wifiEvent) {
        xEventGroupClearBits(s_wifiEvent,
                             WIFI_STA_CONNECTED_BIT | WIFI_STA_FAIL_BIT | WIFI_SCAN_DONE_BIT);
    }
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGI(TAG, "Wi-Fi torn down — free %u int %u largest_int %u",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

void wifi_open(void)
{
    s_wifiEvent = xEventGroupCreate();
    s_staState = WIFI_STA_IDLE;
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_staNetif = esp_netif_create_default_wifi_sta();
    s_apNetif = esp_netif_create_default_wifi_ap();

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL));

    const esp_timer_create_args_t idle_args = {
        .callback = ap_idle_timer_cb,
        .name = "ap_idle",
    };
    ESP_ERROR_CHECK(esp_timer_create(&idle_args, &s_apIdleTimer));

    const esp_timer_create_args_t ble_idle_args = {
        .callback = ble_idle_timer_cb,
        .name = "ble_idle",
    };
    ESP_ERROR_CHECK(esp_timer_create(&ble_idle_args, &s_bleIdleTimer));

    const esp_timer_create_args_t recon_args = {
        .callback = reconnect_timer_cb,
        .name = "sta_recon",
    };
    ESP_ERROR_CHECK(esp_timer_create(&recon_args, &s_reconnectTimer));

    const esp_timer_create_args_t httpd_retry_args = {
        .callback = httpd_retry_timer_cb,
        .name = "httpd_retry",
    };
    ESP_ERROR_CHECK(esp_timer_create(&httpd_retry_args, &s_httpdRetryTimer));

    const esp_timer_create_args_t httpd_wd_args = {
        .callback = httpd_watchdog_timer_cb,
        .name = "httpd_wd",
    };
    ESP_ERROR_CHECK(esp_timer_create(&httpd_wd_args, &s_httpdWatchdogTimer));
    /* 30s: catch SoftAP-close / OOM races that left port 80 down permanently. */
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_httpdWatchdogTimer, 30000000ULL));

    const esp_timer_create_args_t softap_mqtt_args = {
        .callback = softap_mqtt_defer_cb,
        .name = "ap_mqtt",
    };
    ESP_ERROR_CHECK(esp_timer_create(&softap_mqtt_args, &s_softapMqttDeferTimer));

    /* Init Wi-Fi before httpd so STA buffers allocate while largest_int is high. */
    ESP_ERROR_CHECK(wifi_driver_ensure());
}

static bool sta_apply_and_wait(const char *ssid, const char *password, int timeout_ms)
{
    if (!ssid || ssid[0] == '\0') {
        return false;
    }
    if (wifi_driver_ensure() != ESP_OK) {
        return false;
    }
    if (timeout_ms <= 0) {
        timeout_ms = STA_CONNECT_TIMEOUT_MS;
    }

    s_userConnectInFlight = true;
    s_inflightRetryCount = 0;
    s_inflightQuickRetry = false;
    if (s_reconnectTimer) {
        esp_timer_stop(s_reconnectTimer);
    }
    s_staState = WIFI_STA_CONNECTING;

    wifi_mode_t mode = s_softapUp ? WIFI_MODE_APSTA : WIFI_MODE_STA;
    esp_err_t err = esp_wifi_set_mode(mode);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_mode failed: %s", esp_err_to_name(err));
        s_userConnectInFlight = false;
        s_staState = WIFI_STA_IDLE;
        return false;
    }

    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, password ? password : "", sizeof(wc.sta.password));
    /* Allow WPA2/WPA3 personal; open when no password. */
    wc.sta.threshold.authmode = password && password[0]
                                    ? WIFI_AUTH_WPA_WPA2_PSK
                                    : WIFI_AUTH_OPEN;
    err = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sta set_config failed: %s", esp_err_to_name(err));
        s_userConnectInFlight = false;
        s_staState = WIFI_STA_IDLE;
        return false;
    }

    if (!s_wifiStarted) {
        err = esp_wifi_start();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "wifi_start failed: %s", esp_err_to_name(err));
            s_userConnectInFlight = false;
            s_staState = WIFI_STA_IDLE;
            return false;
        }
        s_wifiStarted = true;
    }

    s_staStarted = true;
#if CONFIG_BT_NIMBLE_ENABLED
    const bool ble_coex = s_bleProvUp || ct_prov_ble_is_up();
    if (ble_coex) {
        if (timeout_ms < 30000) {
            timeout_ms = 30000;
        }
        (void)esp_coex_preference_set(ESP_COEX_PREFER_WIFI);
        ct_prov_ble_rf_quiet();
        vTaskDelay(pdMS_TO_TICKS(100));
        ESP_LOGI(TAG, "BLE coex: prefer Wi-Fi for associate ssid=%s", ssid);
    }
#endif
    xEventGroupClearBits(s_wifiEvent, WIFI_STA_CONNECTED_BIT | WIFI_STA_FAIL_BIT);
    esp_wifi_disconnect();
    /* Give the AP a beat after disconnect — 80ms was too tight on some 11ax. */
    vTaskDelay(pdMS_TO_TICKS(STA_CONNECT_SETTLE_MS));
    esp_wifi_connect();

    EventBits_t bits = xEventGroupWaitBits(s_wifiEvent, WIFI_STA_CONNECTED_BIT,
                                           pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    s_userConnectInFlight = false;
    s_inflightQuickRetry = false;
    if (s_reconnectTimer) {
        esp_timer_stop(s_reconnectTimer);
    }
#if CONFIG_BT_NIMBLE_ENABLED
    if (ble_coex) {
        (void)esp_coex_preference_set(ESP_COEX_PREFER_BALANCE);
        ct_prov_ble_rf_restore();
    }
#endif

    if (bits & WIFI_STA_CONNECTED_BIT) {
        s_staState = WIFI_STA_CONNECTED;
        s_reconnectDelayMs = RECONNECT_MIN_MS;
        ESP_LOGI(TAG, "sta associated ssid=%s", ssid);
        return true;
    }

    ESP_LOGW(TAG, "sta connect timeout/failed for ssid=%s", ssid);
    /* Do not keep retrying an unsaved candidate; restore NVS network if any. */
    wifiAttr_t saved;
    cfg_get_wifi_attr(&saved);
    if (saved.ssid[0] != '\0') {
        wifi_config_t wc = { 0 };
        strlcpy((char *)wc.sta.ssid, saved.ssid, sizeof(wc.sta.ssid));
        strlcpy((char *)wc.sta.password, saved.password, sizeof(wc.sta.password));
        wc.sta.threshold.authmode = saved.password[0] ? WIFI_AUTH_WPA_WPA2_PSK : WIFI_AUTH_OPEN;
        esp_wifi_set_config(WIFI_IF_STA, &wc);
        s_staStarted = true;
        s_staState = WIFI_STA_RECONNECTING;
        s_reconnectDelayMs = RECONNECT_MIN_MS;
        reconnect_arm();
    } else {
        if (s_reconnectTimer) {
            esp_timer_stop(s_reconnectTimer);
        }
        esp_wifi_disconnect();
        wifi_config_t clear = { 0 };
        esp_wifi_set_config(WIFI_IF_STA, &clear);
        s_staStarted = false;
        s_staState = WIFI_STA_IDLE;
    }
    return false;
}

bool wifi_sta_try_connect(const char *ssid, const char *password, int timeout_ms)
{
    return sta_apply_and_wait(ssid, password, timeout_ms);
}

bool wifi_sta_reconnect_saved(int timeout_ms)
{
    wifiAttr_t wifi;
    cfg_get_wifi_attr(&wifi);
    if (wifi.ssid[0] == '\0') {
        if (s_reconnectTimer) {
            esp_timer_stop(s_reconnectTimer);
        }
        esp_wifi_disconnect();
        s_staStarted = false;
        s_staState = WIFI_STA_IDLE;
        return false;
    }
    /* timeout_ms == 0: restore config and kick connect without blocking. */
    if (timeout_ms == 0) {
        if (wifi_driver_ensure() != ESP_OK) {
            return false;
        }
        wifi_mode_t mode = s_softapUp ? WIFI_MODE_APSTA : WIFI_MODE_STA;
        if (esp_wifi_set_mode(mode) != ESP_OK) {
            return false;
        }
        wifi_config_t wc = { 0 };
        strlcpy((char *)wc.sta.ssid, wifi.ssid, sizeof(wc.sta.ssid));
        strlcpy((char *)wc.sta.password, wifi.password, sizeof(wc.sta.password));
        wc.sta.threshold.authmode = wifi.password[0] ? WIFI_AUTH_WPA_WPA2_PSK
                                                       : WIFI_AUTH_OPEN;
        if (esp_wifi_set_config(WIFI_IF_STA, &wc) != ESP_OK) {
            return false;
        }
        /* After wifi_driver_teardown (BLE enter), s_wifiStarted is false —
         * connect without start never associates. */
        if (!s_wifiStarted) {
            if (esp_wifi_start() != ESP_OK) {
                ESP_LOGE(TAG, "reconnect_saved: wifi_start failed");
                return false;
            }
            s_wifiStarted = true;
            vTaskDelay(pdMS_TO_TICKS(120));
        }
        s_staStarted = true;
        s_userConnectInFlight = false;
        s_staState = WIFI_STA_RECONNECTING;
        s_reconnectDelayMs = RECONNECT_MIN_MS;
        /* One connect only — arming reconnect here raced a second
         * esp_wifi_connect() while still associating ("sta is connecting"). */
        if (s_reconnectTimer) {
            esp_timer_stop(s_reconnectTimer);
        }
        esp_wifi_connect();
        return false;
    }
    return sta_apply_and_wait(wifi.ssid, wifi.password, timeout_ms);
}

bool wifi_sta_start(void)
{
    wifiAttr_t wifi;
    cfg_get_wifi_attr(&wifi);
    if (wifi.ssid[0] == '\0') {
        return false;
    }
    return sta_apply_and_wait(wifi.ssid, wifi.password, STA_CONNECT_TIMEOUT_MS);
}

wifi_sta_state_t wifi_sta_get_state(void)
{
    if (wifi_sta_is_connected()) {
        return WIFI_STA_CONNECTED;
    }
    return s_staState;
}

const char *wifi_sta_state_str(void)
{
    switch (wifi_sta_get_state()) {
    case WIFI_STA_CONNECTING: return "connecting";
    case WIFI_STA_CONNECTED: return "connected";
    case WIFI_STA_RECONNECTING: return "reconnecting";
    default: return "idle";
    }
}

static uint8_t softap_pick_channel(void)
{
    /* APSTA must share the STA home channel; a fixed SoftAP channel fights STA. */
    wifi_ap_record_t apinfo;
    if (wifi_sta_is_connected() && esp_wifi_sta_get_ap_info(&apinfo) == ESP_OK &&
        apinfo.primary >= 1 && apinfo.primary <= 14) {
        return apinfo.primary;
    }
    return 6;
}

static esp_err_t softap_apply_config(void)
{
    char ssid[32];
    wifi_get_ap_ssid(ssid, sizeof(ssid));

    wifi_config_t ap = { 0 };
    strlcpy((char *)ap.ap.ssid, ssid, sizeof(ap.ap.ssid));
    strlcpy((char *)ap.ap.password, AP_PASS, sizeof(ap.ap.password));
    ap.ap.ssid_len = strlen(ssid);
    ap.ap.channel = softap_pick_channel();
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap.ap.max_connection = 4;
    ap.ap.ssid_hidden = 0;
    ap.ap.beacon_interval = 100;
    /* Optional PMF — required=true breaks many phones on SoftAP. */
    ap.ap.pmf_cfg.capable = true;
    ap.ap.pmf_cfg.required = false;
    ESP_LOGI(TAG, "SoftAP config ssid=%s ch=%u", ssid, (unsigned)ap.ap.channel);
    esp_err_t err = esp_wifi_set_config(WIFI_IF_AP, &ap);
    if (err != ESP_OK) {
        return err;
    }
    /* APSTA + HE STA often demotes SoftAP oddly; lock AP iface to 11bgn so
     * phones can associate (11ax-only SoftAP beacons are a common miss). */
    const uint8_t proto = WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N;
    err = esp_wifi_set_protocol(WIFI_IF_AP, proto);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SoftAP set_protocol BGN: %s (continue)", esp_err_to_name(err));
        return ESP_OK;
    }
    ESP_LOGI(TAG, "SoftAP protocol locked to 11bgn");
    return ESP_OK;
}

/**
 * First-time SoftAP only: paint SSID/pass/URL QR.
 * Later SoftAP sessions must NOT touch the panel — customers may already
 * have art on screen and only need the phone config UI at 192.168.4.1.
 */
static void softap_present_setup_guide_if_needed(void)
{
    if (config_has_wifi()) {
        ESP_LOGI(TAG, "SoftAP up — keep current panel (Wi-Fi already saved)");
        return;
    }
    char ap[32];
    wifi_get_ap_ssid(ap, sizeof(ap));
    if (app_epd_setup_screen_async(ap) != 0) {
        ESP_LOGW(TAG, "setup screen queue failed — fallback text");
        char line1[64];
        char line2[48];
        snprintf(line1, sizeof(line1), "1. Join WiFi: %s", ap);
        snprintf(line2, sizeof(line2), "pass %s", AP_PASS);
        const char *lines[] = { line1, line2, "2. Open http://192.168.4.1" };
        app_epd_boot_screen_async("EK410 SETUP", lines, 3);
    }
    /* SoftAP close restores logo only if user did not push new art. */
    s_softapPaintedGuide = true;
    app_epd_note_system_frame();
}

void wifi_softap_open(void)
{
    if (s_softapUp) {
        ESP_LOGI(TAG, "SoftAP already up — refresh idle timer");
        softap_arm_idle();
        led_set(LEDC_NET_R, LED_BLINK);
        return;
    }

    sleep_cancel(); /* provisioning must not be killed by sleep window */
    /* SoftAP XOR BLE: stop NimBLE in-process, then open SoftAP. */
    if (s_bleProvUp || ct_prov_ble_is_up()) {
        ESP_LOGI(TAG, "SoftAP open — stop BLE then SoftAP");
        cfg_softap_pending_set(true);
        wifi_ble_prov_close(); /* may re-enter here to bring SoftAP up */
        return;
    }
    led_set(LEDC_NET_R, LED_BLINK);
    s_softapOpening = true;
    /* Stop STA backoff so APSTA bring-up is not racing esp_wifi_connect(). */
    if (s_reconnectTimer) {
        esp_timer_stop(s_reconnectTimer);
    }
    /*
     * SoftAP beacon needs contiguous internal RAM. Stop MQTT only when the
     * heap is already tight — SoftAP-first UX should not force "MQTT 未连接"
     * every time the hotspot opens if memory already allows APSTA.
     */
    size_t largest_before =
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    const bool stopped_mqtt =
        mqtt_is_started() && largest_before < SOFTAP_OPEN_KEEP_MQTT_INT;
    if (stopped_mqtt) {
        if (mqtt_stop_wait(5000) != 0) {
            ESP_LOGE(TAG, "SoftAP open aborted — mqtt_stop busy");
            evtlog_event("WIFI", "SoftAP open aborted mqtt busy");
            wifi_set_last_error("mqtt_busy");
            s_softapOpening = false;
            wifi_led_apply_idle_state();
            led_pulse(LEDC_ERR_R, 1200);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(150));
    } else if (mqtt_is_started()) {
        ESP_LOGI(TAG, "SoftAP open — keep MQTT (largest_int=%u)",
                 (unsigned)largest_before);
    }
    if (wifi_driver_ensure() != ESP_OK) {
        s_softapOpening = false;
        wifi_led_apply_idle_state();
        return;
    }
    ESP_LOGI(TAG, "SoftAP open — free heap %u int %u largest_int %u stopped_mqtt=%d",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             stopped_mqtt ? 1 : 0);

    wifi_mode_t mode = s_staStarted ? WIFI_MODE_APSTA : WIFI_MODE_AP;
    esp_err_t err = esp_wifi_set_mode(mode);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_mode failed: %s", esp_err_to_name(err));
        s_softapOpening = false;
        wifi_led_apply_idle_state();
        led_pulse(LEDC_ERR_R, 1200);
        return;
    }
    err = softap_apply_config();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ap set_config failed: %s", esp_err_to_name(err));
        s_softapOpening = false;
        wifi_led_apply_idle_state();
        led_pulse(LEDC_ERR_R, 1200);
        return;
    }
    if (!s_wifiStarted) {
        err = esp_wifi_start();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "wifi_start failed: %s", esp_err_to_name(err));
            s_softapOpening = false;
            wifi_led_apply_idle_state();
            led_pulse(LEDC_ERR_R, 1200);
            return;
        }
        s_wifiStarted = true;
    }

    s_softapUp = true;
    s_softapOpening = false;
    s_apClients = 0;
    wifi_clear_last_error();
    led_set(LEDC_NET_R, LED_BLINK);
    softap_arm_idle();

    /* mqtt_stop above frees internal RAM — SoftAP UI first, MQTT later. */
    if (!http_server_is_up()) {
        http_server_start();
        if (!http_server_is_up()) {
            ESP_LOGW(TAG, "SoftAP up but httpd failed — arm retry");
            httpd_arm_retry(20);
        }
    }

    /* If APSTA briefly dropped STA, kick one reconnect without backoff thrash. */
    if (s_staStarted && !wifi_sta_is_connected()) {
        ESP_LOGI(TAG, "SoftAP up — STA dropped during APSTA, reconnecting");
        s_reconnectDelayMs = RECONNECT_MIN_MS;
        esp_wifi_connect();
        reconnect_arm();
    } else if (wifi_sta_is_connected()) {
        /* Restore MQTT only if we stopped it; never pause SoftAP web first. */
        if (stopped_mqtt || !mqtt_is_started()) {
            softap_arm_mqtt_defer();
        }
    }

    char ssid[32];
    wifi_get_ap_ssid(ssid, sizeof(ssid));
    evtlog_event("WIFI", "SoftAP up ssid=%s", ssid);
    ESP_LOGI(TAG, "SoftAP up: %s / %s -> http://192.168.4.1/ (idle %ds)",
             ssid, AP_PASS, SOFTAP_IDLE_MS / 1000);
    softap_present_setup_guide_if_needed();
}

static void softap_close_internal(void)
{
    if (!s_softapUp) {
        return;
    }
    esp_timer_stop(s_apIdleTimer);
    softap_cancel_mqtt_defer();
    s_softapClosing = true;
    s_softapUp = false;
    s_apClients = 0;

    bool sta_had_ip = wifi_sta_is_connected();

    if (s_staStarted) {
        esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "revert STA mode: %s", esp_err_to_name(err));
        }
        /* APSTA→STA often emits STA_DISCONNECTED; IP may still be valid. */
        if (s_staNetif) {
            esp_netif_ip_info_t ip = { 0 };
            if (esp_netif_get_ip_info(s_staNetif, &ip) == ESP_OK && ip.ip.addr != 0) {
                xEventGroupSetBits(s_wifiEvent, WIFI_STA_CONNECTED_BIT);
                s_staState = WIFI_STA_CONNECTED;
                sta_had_ip = true;
            }
        }
        if (sta_had_ip || wifi_sta_is_connected()) {
            powerAttr_t p;
            cfg_get_power_attr(&p);
            if (p.mode == POWER_MODE_CYCLE && p.sleepMin > 0) {
                sleep_arm_from_config();
            }
        } else {
            s_reconnectDelayMs = RECONNECT_MIN_MS;
            reconnect_arm();
        }
        /* Preserve customer art unless we overwrote it with the setup guide. */
    } else {
        esp_wifi_stop();
        s_wifiStarted = false;
    }

    if (s_softapPaintedGuide) {
        s_softapPaintedGuide = false;
        if (app_epd_has_user_content()) {
            ESP_LOGI(TAG, "SoftAP closed — keep user content (no logo)");
        } else if (app_epd_last_frame_async() == 0) {
            ESP_LOGI(TAG, "SoftAP closed — restore last frame");
        } else {
            ESP_LOGI(TAG, "SoftAP closed — restore CamThink logo");
            if (app_epd_splash_async() != 0) {
                ESP_LOGW(TAG, "logo restore queue failed");
            }
        }
    }

    s_softapClosing = false;
    wifi_led_apply_idle_state();
    /*
     * SoftAP APSTA leaves internal heap fragmented. Bounce httpd (wait for
     * real stop), bring LAN Web back with retry, then MQTT — never leave
     * port 80 dead while the cloud still looks online.
     */
    if (http_server_is_up() || http_server_is_stopping()) {
        http_server_stop();
    }
    wifi_network_services_restore(wifi_sta_is_connected());
    evtlog_event("WIFI", "SoftAP closed sta=%d httpd=%d",
                 wifi_sta_is_connected() ? 1 : 0, http_server_is_up() ? 1 : 0);
    ESP_LOGI(TAG, "SoftAP closed (led=%s httpd=%d)",
             wifi_sta_is_connected() ? "green" : (s_staStarted || config_has_wifi() ? "orange-blink" : "green-idle"),
             http_server_is_up() ? 1 : 0);
}

void wifi_softap_close(void)
{
    softap_close_internal();
}

bool wifi_softap_is_up(void)
{
    return s_softapUp;
}

/**
 * Start BLE CT-Prov in-process. Stops SoftAP/httpd/mqtt first (XOR).
 */
static void ble_enter_restore(bool was_softap)
{
    s_bleEntering = false;
    (void)wifi_driver_ensure();
    if (!http_server_is_up()) {
        http_server_start();
    }
    if (was_softap) {
        ESP_LOGW(TAG, "BLE enter aborted — restoring SoftAP");
        wifi_softap_open();
        if (wifi_softap_is_up()) {
            evtlog_event("BLE", "enter aborted → SoftAP restored");
            return;
        }
        ESP_LOGW(TAG, "SoftAP restore failed — leave STA/MQTT for retry");
        evtlog_event("BLE", "SoftAP restore failed; STA left up");
    }
    if (config_has_wifi()) {
        if (!wifi_sta_is_connected()) {
            wifi_sta_reconnect_saved(0);
        } else {
            mqtt_start();
        }
    }
    wifi_led_apply_idle_state();
    led_pulse(LEDC_ERR_R, 1200);
}

void wifi_ble_prov_enter(void)
{
    if (s_bleProvUp) {
        ble_prov_arm_idle();
        led_set(LEDC_BLE_R, LED_BLINK);
        return;
    }

    /* Lock cyan before teardown — STA_DISCONNECTED must not flash blue. */
    s_bleEntering = true;
    s_httpdPausedForMqtt = false; /* release MQTT pause so restore can restart httpd */
    led_set(LEDC_BLE_R, LED_BLINK);
    sleep_cancel();
    cfg_softap_pending_set(false);
    cfg_ble_prov_boot_set(false);
    const bool was_softap = s_softapUp;
    if (s_softapUp) {
        softap_close_internal();
    }
    if (s_reconnectTimer) {
        esp_timer_stop(s_reconnectTimer);
    }
    ESP_LOGI(TAG, "BLE enter: stop mqtt/wifi/httpd");
    /* Large preview telemetry can hold the MQTT mutex >1.5s — budget retries. */
    if (mqtt_stop_wait(5000) != 0) {
        ESP_LOGE(TAG, "BLE enter aborted — mqtt_stop busy (retry SoftAP/STA)");
        evtlog_event("BLE", "enter aborted mqtt busy");
        wifi_set_last_error("mqtt_busy");
        ble_enter_restore(was_softap);
        return;
    }
    /* Drop Wi-Fi before httpd_off task — otherwise httpd_off often NO_MEM. */
    wifi_driver_teardown();
    http_server_stop(); /* async — do not wait */
    /* Wait for heap coalesce after Wi-Fi teardown (httpd stop is best-effort). */
    for (int i = 0; i < 60; i++) {
        size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
        if (largest >= 20480) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    led_set(LEDC_BLE_R, LED_BLINK);

    ESP_LOGI(TAG, "BLE enter — free heap %u int %u largest_int %u",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

    if (ct_prov_ble_start() != ESP_OK) {
        ESP_LOGE(TAG, "CT-Prov BLE start failed");
        wifi_set_last_error("ble_start_failed");
        ble_enter_restore(was_softap);
        return;
    }

    s_bleProvUp = true;
    s_bleEntering = false;
    wifi_clear_last_error();
    ble_prov_arm_idle();
    led_set(LEDC_BLE_R, LED_BLINK);
    evtlog_event("BLE", "CT-Prov up idle=%us", (unsigned)(ble_idle_ms() / 1000u));
    ESP_LOGI(TAG, "BLE CT-Prov up (idle %us) — cyan; PAGE/idle/session.close/SoftAP to exit",
             (unsigned)(ble_idle_ms() / 1000u));

    /* Warm Wi-Fi STA for scan while NimBLE advertises (coex).
     * Do this BEFORE the CT-Prov worker so wifi_init gets the free block. */
    for (int i = 0; i < 40; i++) {
        size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
        if (largest >= 12288) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    size_t warm_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    if (warm_largest < 10240) {
        ESP_LOGW(TAG, "BLE warm skip — largest_int %u < 10KB (manual SSID ok)",
                 (unsigned)warm_largest);
        wifi_set_last_error("ble_scan_warm_skip");
    } else if (wifi_driver_ensure() == ESP_OK &&
               esp_wifi_set_mode(WIFI_MODE_STA) == ESP_OK) {
        if (!s_wifiStarted && esp_wifi_start() == ESP_OK) {
            s_wifiStarted = true;
            ESP_LOGI(TAG, "BLE warm: Wi-Fi STA idle ready for scan");
        }
    } else {
        ESP_LOGW(TAG, "BLE warm: Wi-Fi unavailable — NeoLink may need manual SSID");
        wifi_set_last_error("ble_scan_warm_skip");
    }

    if (ct_prov_ble_start_worker() != ESP_OK) {
        ESP_LOGW(TAG, "CT-Prov worker deferred fail — GATT cmds unavailable");
        wifi_set_last_error("ble_gatt_unavailable");
    }
}

void wifi_ble_prov_open(void)
{
    wifi_ble_prov_enter();
}

void wifi_ble_prov_close(void)
{
    const bool open_ap = cfg_softap_pending_get();
    cfg_softap_pending_set(false);
    cfg_ble_prov_boot_set(false);
    cfg_ble_idle_sec_set(0); /* clear CI override */

    if (s_bleProvUp || ct_prov_ble_is_up()) {
        s_bleProvClosing = true;
        if (s_bleIdleTimer) {
            esp_timer_stop(s_bleIdleTimer);
        }
        s_bleProvUp = false;
        ct_prov_ble_stop();
        /* Do NOT esp_bt_controller_mem_release — PAGE may re-open BLE later. */
        /* NimBLE teardown often drops STA — wait before reading link state. */
        vTaskDelay(pdMS_TO_TICKS(500));
        s_bleProvClosing = false;
        evtlog_event("BLE", "CT-Prov stopped");
        ESP_LOGI(TAG, "BLE CT-Prov stopped — free heap %u int %u largest_int %u",
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    }

    /* Prefer httpd BEFORE wifi_init — post-NimBLE largest_int is often ~8KB,
     * enough for httpd stack, but default wifi_init would shrink it to ~3KB. */
    for (int i = 0; i < 40; i++) {
        size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
        if (largest >= 7168) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (!http_server_is_up()) {
        size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
        ESP_LOGI(TAG, "BLE exit pre-wifi httpd (largest_int=%u)", (unsigned)largest);
        http_server_start();
    }

    s_wifiLeanBuffers = true;
    (void)wifi_driver_ensure();
    if (open_ap) {
        evtlog_event("BLE", "exit → SoftAP pending");
        ESP_LOGI(TAG, "BLE exit — opening SoftAP (pending)");
        wifi_softap_open();
        return;
    }
    if (!http_server_is_up()) {
        size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
        ESP_LOGI(TAG, "BLE exit post-wifi httpd retry (largest_int=%u)",
                 (unsigned)largest);
        http_server_start();
        if (!http_server_is_up()) {
            httpd_arm_retry(20);
        }
    }
    if (config_has_wifi()) {
        if (!wifi_sta_is_connected()) {
            evtlog_event("BLE", "exit → STA join (orange until IP)");
            ESP_LOGI(TAG, "BLE exit — STA join with saved wifi");
            wifi_sta_reconnect_saved(0);
        } else {
            wifi_network_services_restore(true);
            powerAttr_t p;
            cfg_get_power_attr(&p);
            if (p.mode == POWER_MODE_CYCLE && p.sleepMin > 0) {
                sleep_arm_from_config();
            }
        }
    }
    wifi_led_apply_idle_state();
}

bool wifi_ble_prov_is_up(void)
{
    return s_bleProvUp;
}

bool wifi_ble_prov_busy(void)
{
    return s_bleProvUp || s_bleEntering;
}

void wifi_ble_prov_touch(void)
{
    if (s_bleProvUp) {
        ble_prov_arm_idle();
    }
}

void wifi_ap_start(void)
{
    wifi_softap_open();
}

void wifi_enter_ap_mode(void)
{
    wifi_softap_open();
}

bool wifi_sta_is_connected(void)
{
    return s_staStarted && (xEventGroupGetBits(s_wifiEvent) & WIFI_STA_CONNECTED_BIT);
}

void wifi_get_ip_str(char *buf, int buflen)
{
    buf[0] = '\0';
    if (s_staStarted && s_staNetif) {
        esp_netif_ip_info_t ip;
        if (esp_netif_get_ip_info(s_staNetif, &ip) == ESP_OK && ip.ip.addr != 0) {
            snprintf(buf, buflen, IPSTR, IP2STR(&ip.ip));
            return;
        }
    }
    if (s_softapUp) {
        strlcpy(buf, "192.168.4.1", buflen);
    }
}

void wifi_get_ap_ssid(char *buf, int buflen)
{
    /* NeoLink SoftAP discovery expects legacy `<MODEL>_<MAC6>` (CT-Prov §16).
     * Device display name remains EK410-{deviceId} via getDevInfo / MQTT. */
    ct_prov_softap_ssid(buf, buflen);
}

static bool auth_is_secure(wifi_auth_mode_t mode)
{
    return mode != WIFI_AUTH_OPEN
#if defined(WIFI_AUTH_OWE)
           && mode != WIFI_AUTH_OWE
#endif
        ;
}

int wifi_scan(wifi_scan_ap_t *out, int max, char *errBuf, int errLen)
{
    if (errBuf && errLen > 0) {
        errBuf[0] = '\0';
    }
    if (!out || max <= 0) {
        if (errBuf && errLen > 0) {
            strlcpy(errBuf, "bad args", errLen);
        }
        return -1;
    }
    if (max > WIFI_SCAN_MAX) {
        max = WIFI_SCAN_MAX;
    }

    /* Pause auto-reconnect while we own the radio for a scan. */
    s_userConnectInFlight = true;
    if (s_reconnectTimer) {
        esp_timer_stop(s_reconnectTimer);
    }
    if (wifi_driver_ensure() != ESP_OK) {
        s_userConnectInFlight = false;
        ESP_LOGE(TAG, "scan: wifi_init failed (heap %u int %u largest_int %u)",
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        if (errBuf && errLen > 0) {
            strlcpy(errBuf, "wifi_init failed", errLen);
        }
        return -1;
    }

    wifi_mode_t want = s_softapUp ? WIFI_MODE_APSTA : WIFI_MODE_STA;
    esp_err_t err = esp_wifi_set_mode(want);
    if (err != ESP_OK) {
        s_userConnectInFlight = false;
        if (errBuf && errLen > 0) {
            snprintf(errBuf, errLen, "set_mode: %s", esp_err_to_name(err));
        }
        return -1;
    }
    if (!s_wifiStarted) {
        err = esp_wifi_start();
        if (err != ESP_OK) {
            s_userConnectInFlight = false;
            if (errBuf && errLen > 0) {
                snprintf(errBuf, errLen, "wifi_start: %s", esp_err_to_name(err));
            }
            return -1;
        }
        s_wifiStarted = true;
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    wifi_scan_config_t sc = { 0 };
    sc.ssid = NULL;
    sc.bssid = NULL;
    sc.channel = 0;
    sc.show_hidden = false;
    sc.scan_type = WIFI_SCAN_TYPE_ACTIVE;
#if CONFIG_BT_NIMBLE_ENABLED
    const bool ble_win = s_bleProvUp || ct_prov_ble_is_up();
#else
    const bool ble_win = false;
#endif
    if (!ble_win) {
        sc.scan_time.active.min = 100;
        sc.scan_time.active.max = 300;
    } else {
        /* BLE GATT must survive Rescan: keep dwell short; prefer BT airtime. */
        sc.scan_time.active.min = 30;
        sc.scan_time.active.max = 60;
    }

#if CONFIG_BT_NIMBLE_ENABLED
    if (ble_win) {
        /* Prefer BT so NeoLink's GATT link is not starved during Rescan. */
        (void)esp_coex_preference_set(ESP_COEX_PREFER_BT);
        wifi_ble_prov_touch();
    }
#endif

    xEventGroupClearBits(s_wifiEvent, WIFI_SCAN_DONE_BIT);
    err = esp_wifi_scan_start(&sc, false); /* async */
    if (err != ESP_OK) {
        s_userConnectInFlight = false;
        if (errBuf && errLen > 0) {
            snprintf(errBuf, errLen, "scan: %s", esp_err_to_name(err));
        }
#if CONFIG_BT_NIMBLE_ENABLED
        if (ble_win) {
            (void)esp_coex_preference_set(ESP_COEX_PREFER_BALANCE);
            ct_prov_ble_rf_restore();
        } else
#endif
        {
            reconnect_arm();
        }
        return -1;
    }

    {
        /* Slice the wait so idle timer / GATT keep alive during Rescan. */
        const int wait_ms = ble_win ? 1800 : 12000;
        const int slice_ms = ble_win ? 200 : wait_ms;
        int waited = 0;
        EventBits_t sb = 0;
        while (waited < wait_ms) {
            sb = xEventGroupWaitBits(s_wifiEvent, WIFI_SCAN_DONE_BIT, pdTRUE, pdFALSE,
                                     pdMS_TO_TICKS(slice_ms));
            if (sb & WIFI_SCAN_DONE_BIT) {
                break;
            }
            waited += slice_ms;
#if CONFIG_BT_NIMBLE_ENABLED
            if (ble_win) {
                wifi_ble_prov_touch();
            }
#endif
        }
        if (!(sb & WIFI_SCAN_DONE_BIT)) {
            ESP_LOGW(TAG, "scan wait timeout (%d ms) — stop", wait_ms);
            esp_wifi_scan_stop();
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
#if CONFIG_BT_NIMBLE_ENABLED
    if (ble_win) {
        wifi_ble_prov_touch();
        (void)esp_coex_preference_set(ESP_COEX_PREFER_BALANCE);
        /* Scan often drops the phone link; restart adv so NeoLink can rebind. */
        ct_prov_ble_rf_restore();
    }
#endif

    uint16_t ap_num = 0;
    esp_wifi_scan_get_ap_num(&ap_num);
    if (ap_num == 0) {
        s_userConnectInFlight = false;
        if (!ble_win) {
            reconnect_arm();
        }
        return 0;
    }
    if (ap_num > 48) {
        ap_num = 48;
    }

    wifi_ap_record_t *recs = calloc(ap_num, sizeof(*recs));
    if (!recs) {
        s_userConnectInFlight = false;
        if (errBuf && errLen > 0) {
            strlcpy(errBuf, "oom", errLen);
        }
        if (!ble_win) {
            reconnect_arm();
        }
        return -1;
    }
    err = esp_wifi_scan_get_ap_records(&ap_num, recs);
    if (err != ESP_OK) {
        free(recs);
        s_userConnectInFlight = false;
        if (errBuf && errLen > 0) {
            snprintf(errBuf, errLen, "records: %s", esp_err_to_name(err));
        }
        if (!ble_win) {
            reconnect_arm();
        }
        return -1;
    }

    int n = 0;
    for (uint16_t i = 0; i < ap_num; i++) {
        if (recs[i].ssid[0] == '\0') {
            continue;
        }
        int found = -1;
        for (int j = 0; j < n; j++) {
            if (strncmp(out[j].ssid, (const char *)recs[i].ssid, sizeof(out[j].ssid)) == 0) {
                found = j;
                break;
            }
        }
        if (found >= 0) {
            if (recs[i].rssi > out[found].rssi) {
                out[found].rssi = recs[i].rssi;
                out[found].authmode = (uint8_t)recs[i].authmode;
                out[found].secure = auth_is_secure(recs[i].authmode);
            }
            continue;
        }
        if (n >= max) {
            /* Prefer stronger APs when truncated: replace weakest if stronger. */
            int weakest = 0;
            for (int j = 1; j < n; j++) {
                if (out[j].rssi < out[weakest].rssi) {
                    weakest = j;
                }
            }
            if (recs[i].rssi <= out[weakest].rssi) {
                continue;
            }
            found = weakest;
        } else {
            found = n++;
        }
        memset(&out[found], 0, sizeof(out[found]));
        strlcpy(out[found].ssid, (const char *)recs[i].ssid, sizeof(out[found].ssid));
        out[found].rssi = recs[i].rssi;
        out[found].authmode = (uint8_t)recs[i].authmode;
        out[found].secure = auth_is_secure(recs[i].authmode);
    }
    free(recs);

    /* Sort strongest first. */
    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
            if (out[j].rssi > out[i].rssi) {
                wifi_scan_ap_t tmp = out[i];
                out[i] = out[j];
                out[j] = tmp;
            }
        }
    }

    s_userConnectInFlight = false;
    /* Never arm STA reconnect during BLE prov — it steals RF from GATT/adv. */
    if (!ble_win) {
        reconnect_arm();
    }
#if CONFIG_BT_NIMBLE_ENABLED
    else {
        wifi_ble_prov_touch();
        ct_prov_ble_rf_restore();
    }
#endif
    ESP_LOGI(TAG, "scan found %d unique AP(s)%s", n, ble_win ? " (ble-win)" : "");
    return n;
}
