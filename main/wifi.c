/**
 * @file wifi.c
 * @brief STA with try-then-persist + exponential reconnect; on-demand SoftAP.
 *
 * SoftAP stays down unless opened (button). While open, radio is APSTA so
 * home Wi-Fi can be tried without dropping the config session. SoftAP stays
 * up during config: HTTP activity (wifi_softap_touch) postpones close.
 *
 * Credentials are applied via wifi_sta_try_connect(); NVS write is the
 * caller's job on success. STA disconnect uses backoff reconnect (1s→60s).
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

#include "wifi.h"
#include "config.h"
#include "mqtt.h"
#include "led.h"
#include "sleep.h"
#include "app_epd.h"

#define TAG "-->WIFI"
#define AP_PASS "12345678"
#define STA_CONNECT_TIMEOUT_MS 20000
#define SOFTAP_IDLE_MS         (10 * 60 * 1000)
#define RECONNECT_MIN_MS       1000
#define RECONNECT_MAX_MS       60000

static EventGroupHandle_t s_wifiEvent;
static esp_netif_t *s_staNetif;
static esp_netif_t *s_apNetif;
static esp_timer_handle_t s_apIdleTimer;
static esp_timer_handle_t s_reconnectTimer;
static bool s_staStarted;
static bool s_wifiStarted;
static bool s_softapUp;
static bool s_softapOpening; /* STA→APSTA in progress — suppress reconnect thrash */
static bool s_softapClosing; /* suppress LED thrash while APSTA→STA tears down */
static bool s_softapPaintedGuide; /* restore CamThink logo when SoftAP closes */
static int s_apClients;
static wifi_sta_state_t s_staState;
static int s_reconnectDelayMs = RECONNECT_MIN_MS;
static bool s_userConnectInFlight; /* suppress auto-reconnect during try */

/** Steady LED after SoftAP is gone: green when OK/idle, blue blink while joining. */
static void wifi_led_apply_idle_state(void)
{
    if (s_softapUp) {
        led_set(LEDC_NET_R, LED_BLINK);
        return;
    }
    if (wifi_sta_is_connected()) {
        led_set(LEDC_OK_R, LED_ON);
        return;
    }
    /* Still have a home network to chase */
    if (s_staStarted || config_has_wifi()) {
        led_set(LEDC_NET_R, LED_BLINK);
        return;
    }
    led_set(LEDC_OK_R, LED_ON); /* unprovisioned resting */
}

static void softap_arm_idle(void);
static void softap_close_internal(void);
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

static void reconnect_timer_cb(void *arg)
{
    (void)arg;
    if (!s_staStarted || s_userConnectInFlight) {
        return;
    }
    if (wifi_sta_is_connected()) {
        return;
    }
    ESP_LOGI(TAG, "reconnect attempt (backoff %d ms)", s_reconnectDelayMs);
    s_staState = WIFI_STA_RECONNECTING;
    esp_wifi_connect();
}

static void reconnect_arm(void)
{
    if (!s_reconnectTimer || !s_staStarted || s_userConnectInFlight) {
        return;
    }
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
        /* Do not set FAIL_BIT here — association often disconnects once
         * before GOT_IP; try_connect waits on CONNECTED + timeout only. */
        xEventGroupClearBits(s_wifiEvent, WIFI_STA_CONNECTED_BIT);
        if (!s_userConnectInFlight) {
            s_staState = WIFI_STA_RECONNECTING;
            /* SoftAP open/close itself owns LED + reconnect; ignore APSTA churn. */
            if (!s_softapUp && !s_softapOpening && !s_softapClosing) {
                led_set(LEDC_NET_R, LED_BLINK);
                reconnect_arm();
            }
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        s_apClients++;
        ESP_LOGI(TAG, "SoftAP client +1 (now %d)", s_apClients);
        softap_arm_idle();
        led_set(LEDC_NET_R, LED_BLINK);
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
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&e->ip_info.ip));
        xEventGroupClearBits(s_wifiEvent, WIFI_STA_FAIL_BIT);
        xEventGroupSetBits(s_wifiEvent, WIFI_STA_CONNECTED_BIT);
        s_staState = WIFI_STA_CONNECTED;
        s_reconnectDelayMs = RECONNECT_MIN_MS;
        if (s_reconnectTimer) {
            esp_timer_stop(s_reconnectTimer);
        }
        if (!s_softapUp) {
            led_set(LEDC_OK_R, LED_ON);
        }
        mqtt_start();
        sleep_on_sta_online();
    }
}

void wifi_open(void)
{
    s_wifiEvent = xEventGroupCreate();
    s_staState = WIFI_STA_IDLE;
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_staNetif = esp_netif_create_default_wifi_sta();
    s_apNetif = esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL));

    const esp_timer_create_args_t idle_args = {
        .callback = ap_idle_timer_cb,
        .name = "ap_idle",
    };
    ESP_ERROR_CHECK(esp_timer_create(&idle_args, &s_apIdleTimer));

    const esp_timer_create_args_t recon_args = {
        .callback = reconnect_timer_cb,
        .name = "sta_recon",
    };
    ESP_ERROR_CHECK(esp_timer_create(&recon_args, &s_reconnectTimer));
}

static bool sta_apply_and_wait(const char *ssid, const char *password, int timeout_ms)
{
    if (!ssid || ssid[0] == '\0') {
        return false;
    }
    if (timeout_ms <= 0) {
        timeout_ms = STA_CONNECT_TIMEOUT_MS;
    }

    s_userConnectInFlight = true;
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
    xEventGroupClearBits(s_wifiEvent, WIFI_STA_CONNECTED_BIT | WIFI_STA_FAIL_BIT);
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_wifi_connect();

    EventBits_t bits = xEventGroupWaitBits(s_wifiEvent, WIFI_STA_CONNECTED_BIT,
                                           pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    s_userConnectInFlight = false;

    if (bits & WIFI_STA_CONNECTED_BIT) {
        s_staState = WIFI_STA_CONNECTED;
        s_reconnectDelayMs = RECONNECT_MIN_MS;
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
        wifi_mode_t mode = s_softapUp ? WIFI_MODE_APSTA : WIFI_MODE_STA;
        esp_wifi_set_mode(mode);
        wifi_config_t wc = { 0 };
        strlcpy((char *)wc.sta.ssid, wifi.ssid, sizeof(wc.sta.ssid));
        strlcpy((char *)wc.sta.password, wifi.password, sizeof(wc.sta.password));
        esp_wifi_set_config(WIFI_IF_STA, &wc);
        s_staStarted = true;
        s_staState = WIFI_STA_RECONNECTING;
        s_reconnectDelayMs = RECONNECT_MIN_MS;
        esp_wifi_connect();
        reconnect_arm();
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
    ap.ap.max_connection = 2;
    ESP_LOGI(TAG, "SoftAP config ssid=%s ch=%u", ssid, (unsigned)ap.ap.channel);
    return esp_wifi_set_config(WIFI_IF_AP, &ap);
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
    led_set(LEDC_NET_R, LED_BLINK);
    s_softapOpening = true;
    /* Stop STA backoff so APSTA bring-up is not racing esp_wifi_connect(). */
    if (s_reconnectTimer) {
        esp_timer_stop(s_reconnectTimer);
    }
    /* SoftAP beacon needs a contiguous internal block (IDF min 752B). MQTT
     * task/stack sit in internal RAM — stop it first and let frees settle. */
    mqtt_stop();
    vTaskDelay(pdMS_TO_TICKS(150));
    ESP_LOGI(TAG, "SoftAP open — free heap %u int %u largest_int %u",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

    wifi_mode_t mode = s_staStarted ? WIFI_MODE_APSTA : WIFI_MODE_AP;
    esp_err_t err = esp_wifi_set_mode(mode);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_mode failed: %s", esp_err_to_name(err));
        s_softapOpening = false;
        led_set(LEDC_ERR_R, LED_BLINK);
        return;
    }
    err = softap_apply_config();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ap set_config failed: %s", esp_err_to_name(err));
        s_softapOpening = false;
        led_set(LEDC_ERR_R, LED_BLINK);
        return;
    }
    if (!s_wifiStarted) {
        err = esp_wifi_start();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "wifi_start failed: %s", esp_err_to_name(err));
            s_softapOpening = false;
            led_set(LEDC_ERR_R, LED_BLINK);
            return;
        }
        s_wifiStarted = true;
    }

    s_softapUp = true;
    s_softapOpening = false;
    s_apClients = 0;
    led_set(LEDC_NET_R, LED_BLINK);
    softap_arm_idle();

    /* If APSTA briefly dropped STA, kick one reconnect without backoff thrash. */
    if (s_staStarted && !wifi_sta_is_connected()) {
        ESP_LOGI(TAG, "SoftAP up — STA dropped during APSTA, reconnecting");
        s_reconnectDelayMs = RECONNECT_MIN_MS;
        esp_wifi_connect();
        reconnect_arm();
    }

    char ssid[32];
    wifi_get_ap_ssid(ssid, sizeof(ssid));
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
        } else {
            ESP_LOGI(TAG, "SoftAP closed — restore CamThink logo");
            if (app_epd_splash_async() != 0) {
                ESP_LOGW(TAG, "logo restore queue failed");
            }
        }
    }

    s_softapClosing = false;
    wifi_led_apply_idle_state();
    if (wifi_sta_is_connected()) {
        mqtt_start();
    }
    ESP_LOGI(TAG, "SoftAP closed (led=%s)",
             wifi_sta_is_connected() ? "green" : (s_staStarted || config_has_wifi() ? "blue-blink" : "green-idle"));
}

void wifi_softap_close(void)
{
    softap_close_internal();
}

bool wifi_softap_is_up(void)
{
    return s_softapUp;
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
    /* Prefer burned deviceId so SoftAP matches MQTT / device name (EK410-XXXXXXXX). */
    devInfo_t info;
    cfg_get_dev_info(&info);
    if (info.deviceId[0] != '\0') {
        snprintf(buf, buflen, "EK410-%s", info.deviceId);
        return;
    }
    uint8_t mac[6] = { 0 };
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(buf, buflen, "EK410-%02X%02X", mac[4], mac[5]);
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
    sc.scan_time.active.min = 100;
    sc.scan_time.active.max = 300;

    err = esp_wifi_scan_start(&sc, true);
    if (err != ESP_OK) {
        s_userConnectInFlight = false;
        if (errBuf && errLen > 0) {
            snprintf(errBuf, errLen, "scan: %s", esp_err_to_name(err));
        }
        reconnect_arm();
        return -1;
    }

    uint16_t ap_num = 0;
    esp_wifi_scan_get_ap_num(&ap_num);
    if (ap_num == 0) {
        s_userConnectInFlight = false;
        reconnect_arm();
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
        reconnect_arm();
        return -1;
    }
    err = esp_wifi_scan_get_ap_records(&ap_num, recs);
    if (err != ESP_OK) {
        free(recs);
        s_userConnectInFlight = false;
        if (errBuf && errLen > 0) {
            snprintf(errBuf, errLen, "records: %s", esp_err_to_name(err));
        }
        reconnect_arm();
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
    reconnect_arm();
    ESP_LOGI(TAG, "scan found %d unique AP(s)", n);
    return n;
}
