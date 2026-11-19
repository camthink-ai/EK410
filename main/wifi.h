/**
 * @file wifi.h
 * @brief WiFi station (always-on when provisioned) + on-demand SoftAP.
 *
 * SoftAP is off by default. Long-press opens it; it stays up while the
 * config UI is active (HTTP touch / SoftAP clients). Closes after a long
 * idle or via wifi_softap_close(). STA stays connected (APSTA) while up.
 *
 * STA credentials live in NVS (config). Connect flow: try association
 * first; persist only on success. Disconnect uses exponential backoff.
 */
#ifndef __WIFI_H__
#define __WIFI_H__

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WIFI_STA_CONNECTED_BIT BIT2
#define WIFI_STA_FAIL_BIT      BIT3
#define WIFI_SCAN_DONE_BIT     BIT4

typedef enum {
    WIFI_STA_IDLE = 0,
    WIFI_STA_CONNECTING,
    WIFI_STA_CONNECTED,
    WIFI_STA_RECONNECTING,
} wifi_sta_state_t;

void wifi_open(void);
bool wifi_sta_start(void);

/**
 * Apply credentials and wait for IP (or timeout). Does not write NVS.
 * Uses APSTA when SoftAP is up so the config session stays alive.
 */
bool wifi_sta_try_connect(const char *ssid, const char *password, int timeout_ms);

/* Re-apply credentials already stored in NVS (e.g. after a failed try). */
bool wifi_sta_reconnect_saved(int timeout_ms);

wifi_sta_state_t wifi_sta_get_state(void);
const char *wifi_sta_state_str(void);

/* On-demand SoftAP (idempotent). First-time (no saved Wi-Fi) paints a setup
 * guide; later SoftAP sessions leave the panel alone so customer art is kept.
 * Stays up while the config UI is active;
 * closes after a long idle with no HTTP traffic, or via wifi_softap_close(). */
void wifi_softap_open(void);
void wifi_softap_close(void);
bool wifi_softap_is_up(void);
/* Call from HTTP handlers while SoftAP is up to postpone idle auto-close. */
void wifi_softap_touch(void);

/**
 * On-demand BLE CT-Prov window (PAGE hold). Mutually exclusive with SoftAP
 * in one image: open stops SoftAP/httpd/mqtt then starts NimBLE; SoftAP open
 * stops BLE then brings SoftAP up. No OTA SKU switch / reboot.
 * Exit: PAGE hold again, idle timeout, session.close, SoftAP open.
 */
void wifi_ble_prov_open(void);   /* start BLE CT-Prov in-process (or refresh idle) */
void wifi_ble_prov_enter(void);  /* same as open; kept for call-site clarity */
void wifi_ble_prov_close(void);  /* stop NimBLE; restore httpd/mqtt; SoftAP if pending */
bool wifi_ble_prov_is_up(void);
void wifi_ble_prov_touch(void);

/* Back-compat names used by button/main. */
void wifi_ap_start(void);
void wifi_enter_ap_mode(void);

bool wifi_sta_is_connected(void);
void wifi_get_ip_str(char *buf, int buflen);
void wifi_get_ap_ssid(char *buf, int buflen);

/**
 * Recompute status LED from current SoftAP/BLE/STA/panel state.
 * Call after any transient fault (SoftAP/BLE/OTA fail) so red blink does not stick.
 */
void wifi_led_refresh(void);

/**
 * Restore LAN web + optional MQTT after SoftAP close / BLE exit / TLS pause.
 * Order: wait httpd stop → coalesce heap → httpd (with retry) → mqtt.
 * Safe to call when services are already up (idempotent).
 */
void wifi_network_services_restore(bool want_mqtt);

/**
 * SoftAP+httpd often leaves largest_int too small for the MQTT task stack.
 * Briefly stop httpd (SoftAP radio stays up), then restart it so MQTT can
 * allocate. Call only when mqtt_start is about to defer for heap.
 */
void wifi_coalesce_for_mqtt(void);

/** Last SoftAP/BLE enter failure token for Web ("" when clear). */
const char *wifi_last_error(void);
void wifi_clear_last_error(void);

/**
 * Temporarily disable modem sleep for bulk HTTPS (display_url / wake image).
 * Call with true before the transfer, false when done. Nested calls are counted.
 */
void wifi_throughput_boost(bool enable);

#define WIFI_SCAN_MAX 24

typedef struct {
    char ssid[33];
    int8_t rssi;
    bool secure; /* false = open / OWE */
    uint8_t authmode;
} wifi_scan_ap_t;

/**
 * Blocking active scan. Dedupes by SSID (strongest RSSI kept).
 * SoftAP stays up via APSTA. Returns count written to out[], or -1 on error
 * (errBuf filled when provided).
 */
int wifi_scan(wifi_scan_ap_t *out, int max, char *errBuf, int errLen);

#ifdef __cplusplus
}
#endif

#endif /* __WIFI_H__ */
