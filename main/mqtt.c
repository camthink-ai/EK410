/**
 * @file mqtt.c
 * @brief MQTT / MQTTS client — NeoMind device protocol.
 *
 * Transport comes from NVS (mqtt | mqtts), not from port number.
 * NeoMind standard topics (device_type = ek410_Eink):
 *   publish  device/ek410_Eink/{deviceId}/uplink
 *   sub      device/ek410_Eink/{deviceId}/downlink
 * Uplink JSON is flat: type=telemetry|status|cmd_ack; downlink uses cmd=.
 * LWT also goes to the uplink topic (type=status, retain).
 *
 * Keepalive 30s + app heartbeat every 30s.
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_heap_caps.h"
#include "esp_crt_bundle.h"
#include "mqtt_client.h"
#include "cJSON.h"

#include "mqtt.h"
#include "config.h"
#include "wifi.h"
#include "http.h"
#include "app_epd.h"
#include "evtlog.h"
#include "canvas.h"
#include "battery.h"
#include "sleep.h"
#include "last_frame.h"

#define TAG "-->MQTT"
/* Must match NeoMind-DeviceTypes/types/ek410_Eink.json */
#define NM_DEVICE_TYPE "ek410_Eink"
/* Reassembly cap for fragmented downlink (HTTP canvas stays on HTTP). */
#define CMD_BUF_SIZE 8192
#define CMD_QUEUE_LEN 4
/* ESP32-C61 internal RAM is tight once Wi-Fi is up — 16KB MQTT buffers OOM+panic. */
#define MQTT_RX_BUF 4096
/* Telemetry JSON with preview stays ≤200KB; out buffer with margin. */
#define MQTT_TX_BUF (220 * 1024)
#define MQTT_KEEPALIVE_S 30
#define TELEMETRY_PERIOD_US (30ULL * 1000000ULL)

typedef struct {
    void *client;
    bool isConnected;
    bool started;
    SemaphoreHandle_t mutex;
    QueueHandle_t cmdQueue;
    char topicUp[96];
    char topicDown[96];
    /* reassembled downlink chunks */
    char *rxBuf;
    int rxLen;
    int rxTotal;
} mdMqtt_t;

static mdMqtt_t g_MQ = {0};
static esp_timer_handle_t s_telemetryTimer;
static esp_timer_handle_t s_startRetryTimer;
static char s_lwtMsg[192];
/* Last broker/TLS failure for testMqtt — avoid generic "timeout" when CONNACK refused. */
static char s_lastFail[96];
static volatile bool s_connectRefused;

static void mqtt_clear_fail(void)
{
    s_lastFail[0] = '\0';
    s_connectRefused = false;
}

static void mqtt_set_fail(const char *msg)
{
    if (!msg) {
        return;
    }
    strlcpy(s_lastFail, msg, sizeof(s_lastFail));
}
#define MQTT_START_MIN_HEAP 48000
/* Task stacks must live in internal RAM on ESP32-C61 (PSRAM stacks break create). */
/* TCP client task stack. 2304 overflowed after CONNECTED (Stack protection
 * fault). 3072 is the proven floor; SoftAP may defer start until heap frees. */
#define MQTT_TASK_STACK_TCP 3072
#define MQTT_TASK_STACK_TLS 8192
#define MQTT_TASK_STACK_SLACK 256

static int64_t now_s(void)
{
    return esp_timer_get_time() / 1000000LL;
}

static void rebuild_topics(const char *device_id)
{
    const char *id = (device_id && device_id[0]) ? device_id : "unknown";
    snprintf(g_MQ.topicUp, sizeof(g_MQ.topicUp),
             "device/%s/%s/uplink", NM_DEVICE_TYPE, id);
    snprintf(g_MQ.topicDown, sizeof(g_MQ.topicDown),
             "device/%s/%s/downlink", NM_DEVICE_TYPE, id);
}

static int mqtt_task_stack_for(int transport)
{
    return (transport == MQTT_TRANSPORT_TLS) ? MQTT_TASK_STACK_TLS : MQTT_TASK_STACK_TCP;
}

static size_t mqtt_internal_need(int stack_bytes)
{
    /* Stack alone is not enough: after client_start, DNS/lwIP/phy need more
     * contiguous internal RAM. ~4KB historically aborted; ~8.7KB with httpd
     * up used to defer forever under a 10KB floor. 8KB is the practical floor. */
    size_t need = (size_t)stack_bytes + (size_t)MQTT_TASK_STACK_SLACK;
    if (need < 8192) {
        need = 8192;
    }
    return need;
}

static bool internal_heap_ready_for(int stack_bytes)
{
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    return largest >= mqtt_internal_need(stack_bytes);
}

static void build_broker_uri(char *uri, size_t uri_len, const mqttAttr_t *mqtt)
{
    int port = mqtt->port > 0 ? mqtt->port : mqtt_default_port(mqtt->transport);
    snprintf(uri, uri_len, "%s://%s:%d", mqtt_transport_str(mqtt->transport), mqtt->host, port);
}

/* Caller must hold g_MQ.mutex (or know client lifetime is stable). */
static void publish_raw_unlocked(const char *topic, const char *json, int qos, int retain)
{
    if (!g_MQ.client || !topic || !json) {
        return;
    }
    int len = (int)strlen(json);
    int msg_id = esp_mqtt_client_publish(g_MQ.client, topic, json, len, qos, retain);
    if (msg_id < 0) {
        ESP_LOGE(TAG, "publish fail topic=%s len=%d qos=%d", topic, len, qos);
    } else {
        ESP_LOGI(TAG, "publish ok id=%d topic=%s len=%d", msg_id, topic, len);
    }
}

static void publish_up_unlocked(const char *json, int retain)
{
    if (g_MQ.topicUp[0] == '\0' || !json) {
        return;
    }
    mqttAttr_t mqtt;
    cfg_get_mqtt_attr(&mqtt);
    int qos = (mqtt.qos >= 0 && mqtt.qos <= 2) ? mqtt.qos : 1;
    /* Large telemetry (preview) — QoS0 avoids outbox doubling / publish fail. */
    int len = (int)strlen(json);
    if (len > 16 * 1024) {
        qos = 0;
    }
    publish_raw_unlocked(g_MQ.topicUp, json, qos, retain);
}

/** Publish uplink under mutex so mqtt_stop cannot destroy the client mid-send.
 * Returns false if the mutex could not be taken (payload not sent). */
static bool publish_up(const char *json, int retain)
{
    if (!g_MQ.mutex) {
        ESP_LOGW(TAG, "publish_up: no mutex — drop");
        return false;
    }
    /* cmd_ack / status need a longer wait than telemetry — stop contends here. */
    if (xSemaphoreTake(g_MQ.mutex, pdMS_TO_TICKS(800)) != pdTRUE) {
        ESP_LOGW(TAG, "publish_up: mutex busy — retry");
        if (xSemaphoreTake(g_MQ.mutex, pdMS_TO_TICKS(1200)) != pdTRUE) {
            ESP_LOGE(TAG, "publish_up: mutex busy — drop (%u bytes)",
                     json ? (unsigned)strlen(json) : 0);
            evtlog_event("MQTT", "publish drop (mutex busy)");
            return false;
        }
    }
    publish_up_unlocked(json, retain);
    xSemaphoreGive(g_MQ.mutex);
    return true;
}

static void publish_status(const char *state, int retain)
{
    if (g_MQ.topicUp[0] == '\0') {
        return;
    }
    devInfo_t info;
    mqttAttr_t mqtt;
    cfg_get_dev_info(&info);
    cfg_get_mqtt_attr(&mqtt);
    /* cJSON escapes quotes (panel/devName/detail) — snprintf broke on 7.09". */
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return;
    }
    cJSON_AddStringToObject(root, "type", "status");
    cJSON_AddStringToObject(root, "state", state ? state : "offline");
    cJSON_AddStringToObject(root, "clientId", mqtt.clientId);
    cJSON_AddStringToObject(root, "deviceId", info.deviceId);
    cJSON_AddNumberToObject(root, "ts", (double)now_s());
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json) {
        publish_up(json, retain);
        free(json);
    }
}

void mqtt_publish_telemetry(void)
{
    if (!g_MQ.client || !g_MQ.isConnected) {
        return;
    }
    devInfo_t info;
    mqttAttr_t mqtt;
    wifi_ap_record_t ap;
    int rssi = 0;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        rssi = ap.rssi;
    }
    cfg_get_dev_info(&info);
    cfg_get_mqtt_attr(&mqtt);

    int bat_mv = battery_read_mv();
    int bat = (bat_mv >= 0) ? battery_soc_from_mv(bat_mv) : -1;
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return;
    }
    char ip[48] = "";
    wifi_get_ip_str(ip, sizeof(ip));

    last_frame_meta_t frame = {0};
    (void)last_frame_get_meta(&frame);

    cJSON_AddStringToObject(root, "type", "telemetry");
    cJSON_AddStringToObject(root, "devName", info.devName);
    cJSON_AddStringToObject(root, "deviceId", info.deviceId);
    cJSON_AddStringToObject(root, "clientId", mqtt.clientId);
    cJSON_AddStringToObject(root, "ip", ip);
    cJSON_AddNumberToObject(root, "rssi_dbm", rssi);
    cJSON_AddNumberToObject(root, "heap_free", (double)esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "uptime_s", (double)now_s());
    cJSON_AddStringToObject(root, "fwVersion", info.fwVersion);
    cJSON_AddStringToObject(root, "panel", info.panel);
    if (bat >= 0 && bat_mv >= 0) {
        cJSON_AddNumberToObject(root, "battery_percent", bat);
        cJSON_AddNumberToObject(root, "battery_mv", bat_mv);
    }
    /* Last panel frame meta in cJSON; preview b64 appended manually — cJSON
     * Print would duplicate ~230KB and OOM on C61. Base64 needs no escaping. */
    cJSON_AddBoolToObject(root, "frame_present", frame.present);
    char *prev = NULL;
    if (frame.present) {
        cJSON_AddNumberToObject(root, "frame_seq", (double)frame.seq);
        cJSON_AddNumberToObject(root, "frame_crc32", (double)frame.crc32);
        cJSON_AddNumberToObject(root, "frame_saved_us", (double)frame.saved_us);
        cJSON_AddStringToObject(root, "frame_source",
                                frame.source[0] ? frame.source : "");
        cJSON_AddNumberToObject(root, "frame_bytes", (double)LAST_FRAME_BYTES);
        /* SoftAP+MQTT: skip ~160KB preview so SoftAP UI and broker stay up. */
        if (!wifi_softap_is_up()) {
            last_frame_preview_snap_t snap = { 0 };
            (void)last_frame_preview_snapshot(&snap);
            prev = snap.b64;
            if (prev && prev[0]) {
                cJSON_AddStringToObject(root, "frame_preview_mime", "image/bmp");
                cJSON_AddNumberToObject(root, "frame_preview_w", snap.w);
                cJSON_AddNumberToObject(root, "frame_preview_h", snap.h);
            } else {
                free(prev);
                prev = NULL;
            }
        }
    }
    cJSON_AddNumberToObject(root, "hb", 1);
    /* online tracks Wi-Fi + MQTT — do not claim cloud-ready when STA is down. */
    cJSON_AddBoolToObject(root, "online", wifi_sta_is_connected());
    cJSON_AddBoolToObject(root, "httpServerUp", http_server_is_up());
    cJSON_AddNumberToObject(root, "ts", (double)now_s());
    ESP_LOGI(TAG, "telemetry build frame_present=%d preview_b64=%u",
             frame.present ? 1 : 0,
             (unsigned)(prev ? strlen(prev) : 0));
    char *head = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!head) {
        ESP_LOGE(TAG, "telemetry JSON print OOM");
        free(prev);
        return;
    }
    char *json = head;
    if (prev) {
        size_t hlen = strlen(head);
        while (hlen > 0 && (head[hlen - 1] == '}' || head[hlen - 1] == ' ' ||
                            head[hlen - 1] == '\n' || head[hlen - 1] == '\r')) {
            hlen--;
        }
        size_t plen = strlen(prev);
        const char *prefix = ",\"frame_preview_b64\":\"";
        const char *suffix = "\"}";
        size_t out_len = hlen + strlen(prefix) + plen + strlen(suffix) + 1;
        char *out = heap_caps_malloc(out_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!out) {
            out = malloc(out_len);
        }
        if (!out) {
            ESP_LOGE(TAG, "telemetry preview append OOM (need %u)", (unsigned)out_len);
            free(head);
            free(prev);
            return;
        }
        memcpy(out, head, hlen);
        memcpy(out + hlen, prefix, strlen(prefix));
        memcpy(out + hlen + strlen(prefix), prev, plen);
        memcpy(out + hlen + strlen(prefix) + plen, suffix, strlen(suffix) + 1);
        free(head);
        free(prev);
        prev = NULL;
        json = out;
    }
    /* Hold client mutex across publish so mqtt_stop cannot destroy mid-send;
     * a contended stop returns busy and callers must not persist new broker. */
    if (!g_MQ.mutex ||
        xSemaphoreTake(g_MQ.mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        free(json);
        return;
    }
    if (g_MQ.client && g_MQ.isConnected) {
        publish_up_unlocked(json, 0);
    }
    xSemaphoreGive(g_MQ.mutex);
    free(json);
}

void mqtt_publish_cmd_ack(const char *cmd, const char *status, const char *detail)
{
    if (!g_MQ.client) {
        return;
    }
    devInfo_t info;
    cfg_get_dev_info(&info);
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return;
    }
    cJSON_AddStringToObject(root, "type", "cmd_ack");
    cJSON_AddStringToObject(root, "devName", info.devName);
    cJSON_AddStringToObject(root, "deviceId", info.deviceId);
    cJSON_AddStringToObject(root, "last_cmd", cmd ? cmd : "");
    cJSON_AddStringToObject(root, "cmd_status", status ? status : "");
    cJSON_AddStringToObject(root, "detail", detail ? detail : "");
    cJSON_AddNumberToObject(root, "ts", (double)now_s());
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json) {
        if (!publish_up(json, 0)) {
            ESP_LOGW(TAG, "cmd_ack not sent (%s/%s)",
                     cmd ? cmd : "", status ? status : "");
        }
        free(json);
    }
}

/* ---------- command execution ---------- */

static void handle_command(const char *raw, int len)
{
    cJSON *root = cJSON_ParseWithLength(raw, len);
    if (!root) {
        ESP_LOGW(TAG, "bad JSON on downlink");
        mqtt_publish_cmd_ack("parse", "error", "invalid JSON");
        return;
    }
    const char *cmd = "";
    cJSON *jcmd = cJSON_GetObjectItem(root, "cmd");
    if (cJSON_IsString(jcmd)) {
        cmd = jcmd->valuestring;
    }
    cJSON *p = cJSON_GetObjectItem(root, "params");
    if (!p) {
        p = root;
    }

    if (!strcasecmp(cmd, "ping") || !strcasecmp(cmd, "heartbeat")) {
        mqtt_publish_telemetry();
        mqtt_publish_cmd_ack(cmd, "ok", "pong");
    } else if (!strcasecmp(cmd, "clear")) {
        cJSON *c = cJSON_GetObjectItem(p, "color");
        app_epd_mqtt_ack_next(cmd);
        int rc = app_epd_fill_async(cJSON_IsString(c) ? canvas_color(c->valuestring) : 0x11);
        /* Match HTTP: accept → status "queued"; failure → "error"+token. */
        if (rc == 0) {
            mqtt_publish_cmd_ack(cmd, "queued", app_epd_queue_err(0));
        } else {
            app_epd_mqtt_ack_next(NULL);
            mqtt_publish_cmd_ack(cmd, "error", app_epd_queue_err(rc));
        }
    } else if (!strcasecmp(cmd, "render_text")) {
        cJSON *t = cJSON_GetObjectItem(p, "text");
        cJSON *c = cJSON_GetObjectItem(p, "color");
        cJSON *bg = cJSON_GetObjectItem(p, "bg");
        cJSON *sc = cJSON_GetObjectItem(p, "scale");
        if (!cJSON_IsString(t)) {
            mqtt_publish_cmd_ack(cmd, "error", "missing text");
        } else {
            /* Heap — wrap runs on epd_render task; only copy raw paragraph here. */
            espd_render_t *render = calloc(1, sizeof(*render));
            if (!render) {
                mqtt_publish_cmd_ack(cmd, "error", "oom");
                cJSON_Delete(root);
                return;
            }
            size_t tlen = strlen(t->valuestring);
            canvas_utf8_strlcpy(render->text, t->valuestring, sizeof(render->text));
            render->fg = cJSON_IsString(c) ? canvas_color(c->valuestring) : 0x00;
            render->bg = cJSON_IsString(bg) ? canvas_color(bg->valuestring) : 0x11;
            render->scale = cJSON_IsNumber(sc) ? sc->valueint : 3;
            ESP_LOGI(TAG, "render_text queue %u bytes (raw cap %u) scale=%d",
                     (unsigned)tlen, (unsigned)sizeof(render->text) - 1, render->scale);
            if (tlen >= sizeof(render->text)) {
                ESP_LOGW(TAG, "render_text truncated to %u bytes",
                         (unsigned)sizeof(render->text) - 1);
            }
            app_epd_mqtt_ack_next(cmd);
            int rc = app_epd_render_async(render);
            free(render);
            if (rc == 0) {
                mqtt_publish_cmd_ack(cmd, "queued", app_epd_queue_err(0));
            } else {
                app_epd_mqtt_ack_next(NULL);
                mqtt_publish_cmd_ack(cmd, "error", app_epd_queue_err(rc));
            }
        }
    } else if (!strcasecmp(cmd, "render_canvas")) {
        canvas_t *cv = malloc(sizeof(canvas_t));
        char err[64];
        if (!cv) {
            mqtt_publish_cmd_ack(cmd, "error", "oom");
        } else if (canvas_parse_json(p, cv, err, sizeof(err)) != 0) {
            mqtt_publish_cmd_ack(cmd, "error", err);
            free(cv);
        } else {
            app_epd_mqtt_ack_next(cmd);
            int rc = app_epd_canvas_async(cv);
            if (rc == 0) {
                mqtt_publish_cmd_ack(cmd, "queued", app_epd_queue_err(0));
            } else {
                app_epd_mqtt_ack_next(NULL);
                mqtt_publish_cmd_ack(cmd, "error", app_epd_queue_err(rc));
            }
            free(cv);
        }
    } else if (!strcasecmp(cmd, "display_url")) {
        cJSON *u = cJSON_GetObjectItem(p, "url");
        if (!cJSON_IsString(u) || !u->valuestring[0]) {
            mqtt_publish_cmd_ack(cmd, "error", "missing url");
        } else if (strlen(u->valuestring) >= 512) {
            mqtt_publish_cmd_ack(cmd, "error", "url too long");
        } else {
            ESP_LOGI(TAG, "display_url queued: %.80s%s", u->valuestring,
                     strlen(u->valuestring) > 80 ? "…" : "");
            int rc = app_epd_url_async(u->valuestring);
            if (rc == 0) {
                mqtt_publish_cmd_ack(cmd, "queued", app_epd_queue_err(0));
            } else {
                mqtt_publish_cmd_ack(cmd, "error", app_epd_queue_err(rc));
            }
        }
    } else if (!strcasecmp(cmd, "reboot")) {
        mqtt_publish_cmd_ack(cmd, "ok", "rebooting");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    } else if (!strcasecmp(cmd, "sleep_now") || !strcasecmp(cmd, "sleep")) {
        if (wifi_softap_is_up()) {
            mqtt_publish_cmd_ack(cmd, "error", "softap_busy");
        } else if (!sleep_now()) {
            mqtt_publish_cmd_ack(cmd, "error", "sleep_failed");
        } else {
            /* sleep_go waits on the panel first — MQTT still up for this ack. */
            mqtt_publish_cmd_ack(cmd, "ok", "sleeping");
        }
    } else if (!strcasecmp(cmd, "set_power")) {
        powerAttr_t pwr;
        cfg_get_power_attr(&pwr);
        cJSON *m = cJSON_GetObjectItem(p, "mode");
        cJSON *sm = cJSON_GetObjectItem(p, "sleepMin");
        cJSON *ws = cJSON_GetObjectItem(p, "wakeSec");
        cJSON *wu = cJSON_GetObjectItem(p, "wakeUrl");
        cJSON *iq = cJSON_GetObjectItem(p, "imageQuality");
        if (cJSON_IsString(m)) {
            pwr.mode = power_mode_from_str(m->valuestring);
        }
        if (cJSON_IsNumber(sm)) {
            pwr.sleepMin = sm->valueint < 0 ? 0 : sm->valueint;
        }
        if (cJSON_IsNumber(ws)) {
            pwr.wakeSec = ws->valueint;
        }
        if (cJSON_IsString(wu)) {
            strlcpy(pwr.wakeUrl, wu->valuestring, sizeof(pwr.wakeUrl));
        }
        if (cJSON_IsString(iq)) {
            pwr.imageQuality = img_quality_from_str(iq->valuestring);
        } else if (cJSON_IsNumber(iq)) {
            pwr.imageQuality = (iq->valueint == 0) ? IMG_QUALITY_SPEED : IMG_QUALITY_QUALITY;
        }
        cfg_set_power_attr(&pwr);
        cfg_get_power_attr(&pwr);
        if (pwr.mode == POWER_MODE_CYCLE) {
            if (!wifi_softap_is_up() && wifi_sta_is_connected()) {
                sleep_arm_from_config();
            } else {
                sleep_cancel();
            }
            mqtt_publish_cmd_ack(cmd, "ok", power_mode_str(pwr.mode));
        } else if (pwr.mode == POWER_MODE_HIBERNATE) {
            if (wifi_softap_is_up()) {
                mqtt_publish_cmd_ack(cmd, "error", "softap_busy");
            } else if (!sleep_now()) {
                mqtt_publish_cmd_ack(cmd, "error", "sleep_failed");
            } else {
                mqtt_publish_cmd_ack(cmd, "ok", "hibernating");
            }
        } else {
            sleep_cancel();
            mqtt_publish_cmd_ack(cmd, "ok", "always");
        }
    } else {
        mqtt_publish_cmd_ack(cmd && cmd[0] ? cmd : "(empty)", "error", "unknown command");
    }
    cJSON_Delete(root);
}

static void cmd_task(void *arg)
{
    char *buf;
    while (xQueueReceive(g_MQ.cmdQueue, &buf, portMAX_DELAY)) {
        if (buf == NULL) {
            continue;
        }
        ESP_LOGI(TAG, "executing command (%d bytes): %.160s%s",
                 (int)strlen(buf), buf, strlen(buf) > 160 ? "…" : "");
        handle_command(buf, strlen(buf));
        free(buf);
    }
}

/* Take ownership of buf (NUL room at buf[len]). Worker frees after handle. */
static void enqueue_command_owned(char *buf, int len)
{
    if (!buf) {
        return;
    }
    if (len <= 0 || len >= CMD_BUF_SIZE) {
        ESP_LOGW(TAG, "command size %d rejected", len);
        mqtt_publish_cmd_ack("(queue)", "error", "command too large");
        free(buf);
        return;
    }
    buf[len] = '\0';
    if (xQueueSend(g_MQ.cmdQueue, &buf, 0) != pdTRUE) {
        ESP_LOGW(TAG, "command queue full — drop %d bytes", len);
        mqtt_publish_cmd_ack("(queue)", "error", "device busy");
        free(buf);
    }
}

static void enqueue_command_copy(const char *data, int len)
{
    if (len <= 0 || len >= CMD_BUF_SIZE) {
        ESP_LOGW(TAG, "command size %d rejected", len);
        mqtt_publish_cmd_ack("(queue)", "error", "command too large");
        return;
    }
    char *copy = malloc((size_t)len + 1);
    if (!copy) {
        ESP_LOGE(TAG, "command OOM (%d bytes)", len);
        mqtt_publish_cmd_ack("(queue)", "error", "out of memory");
        return;
    }
    memcpy(copy, data, (size_t)len);
    enqueue_command_owned(copy, len);
}

/* Caller must hold g_MQ.mutex (same lock as mqtt_stop → destroy). */
static void rx_reset_unlocked(void)
{
    free(g_MQ.rxBuf);
    g_MQ.rxBuf = NULL;
    g_MQ.rxLen = 0;
    g_MQ.rxTotal = 0;
}

static void rx_reset(void)
{
    if (!g_MQ.mutex) {
        rx_reset_unlocked();
        return;
    }
    if (xSemaphoreTake(g_MQ.mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        /* Never free rxBuf without the lock — mqtt_stop / fragment path own it. */
        ESP_LOGW(TAG, "rx_reset: mutex busy — skip (stop path will clear)");
        return;
    }
    rx_reset_unlocked();
    xSemaphoreGive(g_MQ.mutex);
}

static void handle_data_event(esp_mqtt_event_handle_t event)
{
    /* Single-chunk fast path — must copy; MQTT buffer is transient. */
    if (event->total_data_len > 0 &&
        event->current_data_offset == 0 &&
        event->data_len == event->total_data_len) {
        enqueue_command_copy(event->data, event->data_len);
        return;
    }

    /* Fragment reassembly under mutex — mqtt_stop must not free mid-memcpy. */
    if (!g_MQ.mutex ||
        xSemaphoreTake(g_MQ.mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        ESP_LOGW(TAG, "fragment: mutex busy — drop chunk");
        mqtt_publish_cmd_ack("(queue)", "error", "fragment desync");
        return;
    }

    if (event->current_data_offset == 0) {
        rx_reset_unlocked();
        if (event->total_data_len <= 0 || event->total_data_len >= CMD_BUF_SIZE) {
            ESP_LOGW(TAG, "command too large (%d)", event->total_data_len);
            xSemaphoreGive(g_MQ.mutex);
            mqtt_publish_cmd_ack("(queue)", "error", "command too large");
            return;
        }
        g_MQ.rxBuf = malloc((size_t)event->total_data_len + 1);
        if (!g_MQ.rxBuf) {
            ESP_LOGE(TAG, "fragment OOM (%d)", event->total_data_len);
            xSemaphoreGive(g_MQ.mutex);
            mqtt_publish_cmd_ack("(queue)", "error", "out of memory");
            return;
        }
        g_MQ.rxTotal = event->total_data_len;
        g_MQ.rxLen = 0;
    }
    if (!g_MQ.rxBuf || g_MQ.rxTotal <= 0) {
        ESP_LOGW(TAG, "chunk with no reassembly (off=%d) — drop",
                 event->current_data_offset);
        xSemaphoreGive(g_MQ.mutex);
        mqtt_publish_cmd_ack("(queue)", "error", "fragment desync");
        return;
    }
    if (event->current_data_offset != g_MQ.rxLen) {
        ESP_LOGW(TAG, "chunk gap %d!=%d — drop", event->current_data_offset, g_MQ.rxLen);
        rx_reset_unlocked();
        xSemaphoreGive(g_MQ.mutex);
        mqtt_publish_cmd_ack("(queue)", "error", "fragment desync");
        return;
    }
    if (g_MQ.rxLen + event->data_len > g_MQ.rxTotal) {
        ESP_LOGW(TAG, "chunk overflow %d+%d>%d — drop",
                 g_MQ.rxLen, event->data_len, g_MQ.rxTotal);
        rx_reset_unlocked();
        xSemaphoreGive(g_MQ.mutex);
        mqtt_publish_cmd_ack("(queue)", "error", "fragment desync");
        return;
    }
    memcpy(g_MQ.rxBuf + g_MQ.rxLen, event->data, (size_t)event->data_len);
    g_MQ.rxLen += event->data_len;
    char *owned = NULL;
    int total = 0;
    if (g_MQ.rxLen >= g_MQ.rxTotal) {
        owned = g_MQ.rxBuf;
        total = g_MQ.rxTotal;
        g_MQ.rxBuf = NULL;
        g_MQ.rxLen = 0;
        g_MQ.rxTotal = 0;
    }
    xSemaphoreGive(g_MQ.mutex);
    if (owned) {
        enqueue_command_owned(owned, total);
    }
}

/* Destroy client outside the MQTT event callback (unsafe to destroy in-handler). */
static void mqtt_zombie_stop_task(void *arg)
{
    (void)arg;
    ESP_LOGW(TAG, "tearing down MQTT session after subscribe failure");
    (void)mqtt_stop_wait(5000);
    vTaskDelete(NULL);
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t id, void *data)
{
    esp_mqtt_event_handle_t event = data;
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED: {
        mqttAttr_t mqtt;
        cfg_get_mqtt_attr(&mqtt);
        int qos = (mqtt.qos >= 0 && mqtt.qos <= 2) ? mqtt.qos : 1;
        ESP_LOGI(TAG, "connected — sub %s (qos %d)", g_MQ.topicDown, qos);
        int sub = esp_mqtt_client_subscribe(g_MQ.client, g_MQ.topicDown, qos);
        if (sub < 0) {
            ESP_LOGE(TAG, "subscribe failed (down=%d) — destroy session", sub);
            evtlog_event("MQTT", "subscribe fail rc=%d", sub);
            mqtt_set_fail("subscribe failed");
            g_MQ.isConnected = false;
            /* Disconnect now; deferred stop clears client so mqtt_is_started is false. */
            if (g_MQ.client) {
                (void)esp_mqtt_client_disconnect(g_MQ.client);
            }
            if (xTaskCreate(mqtt_zombie_stop_task, "mqtt_zomb", 3072, NULL, 5, NULL) != pdPASS) {
                ESP_LOGE(TAG, "zombie stop task create failed — client may linger");
            }
            break;
        }
        g_MQ.isConnected = true;
        mqtt_clear_fail();
        evtlog_event("MQTT", "connected sub=%s qos=%d", g_MQ.topicDown, qos);
        publish_status("online", 1);
        /* Do NOT publish full telemetry (≈160KB preview) here — it races
         * PAGE/BLE enter, fragments internal RAM, and causes NimBLE/Wi-Fi
         * NO_MEM + red LED. Periodic timer sends telemetry shortly after. */
        break;
    }
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "disconnected");
        evtlog_event("MQTT", "disconnected");
        g_MQ.isConnected = false;
        rx_reset();
        break;
    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "downlink [%.*s] %d/%d", event->topic_len, event->topic,
                 event->current_data_offset + event->data_len, event->total_data_len);
        sleep_kick(); /* stay awake while platform is sending commands */
        handle_data_event(event);
        break;
    case MQTT_EVENT_ERROR:
        if (event->error_handle) {
            ESP_LOGE(TAG, "error type=%d tls=0x%x sock=%d",
                     event->error_handle->error_type,
                     event->error_handle->esp_tls_last_esp_err,
                     event->error_handle->esp_tls_stack_err);
            evtlog_event("MQTT", "error type=%d tls=0x%x",
                         event->error_handle->error_type,
                         event->error_handle->esp_tls_last_esp_err);
            if (event->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
                s_connectRefused = true;
                switch (event->error_handle->connect_return_code) {
                case MQTT_CONNECTION_REFUSE_BAD_USERNAME:
                    mqtt_set_fail("bad username or password");
                    break;
                case MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED:
                    mqtt_set_fail("not authorized");
                    break;
                case MQTT_CONNECTION_REFUSE_ID_REJECTED:
                    mqtt_set_fail("client id rejected");
                    break;
                case MQTT_CONNECTION_REFUSE_SERVER_UNAVAILABLE:
                    mqtt_set_fail("broker unavailable");
                    break;
                case MQTT_CONNECTION_REFUSE_PROTOCOL:
                    mqtt_set_fail("protocol refused");
                    break;
                default:
                    mqtt_set_fail("connection refused by broker");
                    break;
                }
            } else if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
                mqtt_set_fail("TLS/TCP transport failed");
            }
        } else {
            ESP_LOGE(TAG, "error (no detail)");
            evtlog_event("MQTT", "error (no detail)");
        }
        break;
    default:
        break;
    }
}

static void telemetry_timer_cb(void *arg)
{
    (void)arg;
    mqtt_publish_telemetry();
}

static void mqtt_start_retry_cb(void *arg);
static void mqtt_bringup_task(void *arg);

static SemaphoreHandle_t s_bringupSem;
static volatile bool s_bringupPending;
static bool s_mqttInfraOk;

bool mqtt_infra_ok(void)
{
    return s_mqttInfraOk;
}

const char *mqtt_last_error(void)
{
    return s_lastFail;
}

static void mqtt_open_teardown(TaskHandle_t cmd_h, TaskHandle_t up_h)
{
    if (up_h) {
        vTaskDelete(up_h);
    }
    if (cmd_h) {
        vTaskDelete(cmd_h);
    }
    if (s_startRetryTimer) {
        esp_timer_stop(s_startRetryTimer);
        esp_timer_delete(s_startRetryTimer);
        s_startRetryTimer = NULL;
    }
    if (s_telemetryTimer) {
        esp_timer_stop(s_telemetryTimer);
        esp_timer_delete(s_telemetryTimer);
        s_telemetryTimer = NULL;
    }
    if (s_bringupSem) {
        vSemaphoreDelete(s_bringupSem);
        s_bringupSem = NULL;
    }
    if (g_MQ.cmdQueue) {
        vQueueDelete(g_MQ.cmdQueue);
        g_MQ.cmdQueue = NULL;
    }
    if (g_MQ.mutex) {
        vSemaphoreDelete(g_MQ.mutex);
        g_MQ.mutex = NULL;
    }
}

void mqtt_open(void)
{
    memset(&g_MQ, 0, sizeof(mdMqtt_t));
    s_mqttInfraOk = false;
    s_telemetryTimer = NULL;
    s_startRetryTimer = NULL;
    s_bringupSem = NULL;

    g_MQ.mutex = xSemaphoreCreateMutex();
    g_MQ.cmdQueue = xQueueCreate(CMD_QUEUE_LEN, sizeof(char *));
    s_bringupSem = xSemaphoreCreateBinary();
    if (!g_MQ.mutex || !g_MQ.cmdQueue || !s_bringupSem) {
        ESP_LOGE(TAG, "mqtt_open: mutex/queue/sem alloc failed");
        mqtt_open_teardown(NULL, NULL);
        return;
    }
    /* Keep lean — created at boot while internal heap is whole.
     * Spawning 6KB mqtt_up later (SoftAP/test) hits NO_MEM and testMqtt hangs. */
    TaskHandle_t cmd_h = NULL;
    TaskHandle_t up_h = NULL;
    if (xTaskCreate(cmd_task, "mqtt_cmd", 5120, NULL, 5, &cmd_h) != pdPASS) {
        ESP_LOGE(TAG, "mqtt_open: mqtt_cmd task create failed");
        mqtt_open_teardown(NULL, NULL);
        return;
    }
    if (xTaskCreate(mqtt_bringup_task, "mqtt_up", 6144, NULL, 5, &up_h) != pdPASS) {
        ESP_LOGE(TAG, "mqtt_open: mqtt_up task create failed");
        mqtt_open_teardown(cmd_h, NULL);
        return;
    }

    const esp_timer_create_args_t timerArgs = {
        .name = "telemetry", .callback = telemetry_timer_cb,
    };
    const esp_timer_create_args_t retryArgs = {
        .name = "mqtt_start", .callback = mqtt_start_retry_cb,
    };
    if (esp_timer_create(&timerArgs, &s_telemetryTimer) != ESP_OK) {
        ESP_LOGE(TAG, "mqtt_open: telemetry timer create failed");
        mqtt_open_teardown(cmd_h, up_h);
        return;
    }
    if (esp_timer_create(&retryArgs, &s_startRetryTimer) != ESP_OK) {
        ESP_LOGE(TAG, "mqtt_open: start-retry timer create failed");
        mqtt_open_teardown(cmd_h, up_h);
        return;
    }
    s_mqttInfraOk = true;
}

static bool mqtt_esp_start_locked(void)
{
    mqttAttr_t mqtt;
    cfg_get_mqtt_attr(&mqtt);
    if (mqtt.host[0] == '\0') {
        ESP_LOGW(TAG, "skip start — broker host empty");
        return false;
    }
    if (mqtt.clientId[0] == '\0') {
        ESP_LOGW(TAG, "skip start — clientId empty");
        return false;
    }

    {
        devInfo_t info;
        cfg_get_dev_info(&info);
        rebuild_topics(info.deviceId);
    }

    int port = mqtt.port > 0 ? mqtt.port : mqtt_default_port(mqtt.transport);
    mqtt.port = port;
    const bool use_tls = (mqtt.transport == MQTT_TRANSPORT_TLS);
    const int task_stack = mqtt_task_stack_for(mqtt.transport);

    char uri[144];
    build_broker_uri(uri, sizeof(uri), &mqtt);

    {
        devInfo_t info;
        cfg_get_dev_info(&info);
        /* LWT buffer is fixed; build via cJSON then copy (escape-safe). */
        cJSON *lwt = cJSON_CreateObject();
        if (lwt) {
            cJSON_AddStringToObject(lwt, "type", "status");
            cJSON_AddStringToObject(lwt, "state", "offline");
            cJSON_AddStringToObject(lwt, "clientId", mqtt.clientId);
            cJSON_AddStringToObject(lwt, "deviceId", info.deviceId);
            cJSON_AddStringToObject(lwt, "reason", "lwt");
            char *s = cJSON_PrintUnformatted(lwt);
            cJSON_Delete(lwt);
            if (s) {
                strlcpy(s_lwtMsg, s, sizeof(s_lwtMsg));
                free(s);
            }
        }
    }

    size_t largest_int = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "init %s (heap %u int %u largest_int %u stack %d)", uri,
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)largest_int, task_stack);
    if (!internal_heap_ready_for(task_stack)) {
        ESP_LOGW(TAG, "defer start — largest internal block %u < %u",
                 (unsigned)largest_int,
                 (unsigned)(task_stack + MQTT_TASK_STACK_SLACK));
        return false;
    }

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = uri,
        .credentials.client_id = mqtt.clientId,
        .session.keepalive = MQTT_KEEPALIVE_S,
        .session.disable_clean_session = false,
        .session.last_will.topic = g_MQ.topicUp,
        .session.last_will.msg = s_lwtMsg,
        .session.last_will.msg_len = 0,
        .session.last_will.qos = 1,
        .session.last_will.retain = true,
        .network.reconnect_timeout_ms = 5000,
        .network.timeout_ms = use_tls ? 20000 : 15000,
        .buffer.size = MQTT_RX_BUF,
        .buffer.out_size = MQTT_TX_BUF,
        .task.stack_size = task_stack,
        .task.priority = 5,
    };
    if (use_tls) {
        if (!mqtt.tlsVerify) {
            /* Requires CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY — lab / self-signed. */
            cfg.broker.verification.skip_cert_common_name_check = true;
            ESP_LOGW(TAG, "MQTTS insecure — server certificate verify disabled");
        } else if (cfg_mqtt_ca_pem()[0] != '\0') {
            cfg.broker.verification.certificate = cfg_mqtt_ca_pem();
            ESP_LOGI(TAG, "MQTTS using custom CA PEM (%u bytes)",
                     (unsigned)strlen(cfg_mqtt_ca_pem()));
        } else {
            cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
        }
    }
    if (mqtt.username[0] != '\0') {
        cfg.credentials.username = mqtt.username;
        cfg.credentials.authentication.password = mqtt.password;
    }

    g_MQ.client = esp_mqtt_client_init(&cfg);
    if (!g_MQ.client) {
        ESP_LOGE(TAG, "client init failed (%s) free_heap=%u",
                 uri, (unsigned)esp_get_free_heap_size());
        return false;
    }
    esp_err_t err = esp_mqtt_client_register_event(g_MQ.client, ESP_EVENT_ANY_ID,
                                                   mqtt_event_handler, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register_event failed: %s", esp_err_to_name(err));
        esp_mqtt_client_destroy(g_MQ.client);
        g_MQ.client = NULL;
        return false;
    }
    err = esp_mqtt_client_start(g_MQ.client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "client start failed: %s (largest_int %u)",
                 esp_err_to_name(err),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        esp_mqtt_client_destroy(g_MQ.client);
        g_MQ.client = NULL;
        return false;
    }
    g_MQ.started = true;
    ESP_LOGI(TAG, "started %s up=%s down=%s", uri, g_MQ.topicUp, g_MQ.topicDown);
    return true;
}

/**
 * Heavy MQTT bring-up must NOT run on sys_evt (GOT_IP) — that task has ~2.8KB
 * stack and Stack-protection-fault reboots the device. SoftAP heap coalesce is
 * also too big for sys_evt. Worker is created once in mqtt_open().
 */
static void mqtt_bringup_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (!s_bringupSem || xSemaphoreTake(s_bringupSem, portMAX_DELAY) != pdTRUE) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        s_bringupPending = false;

        if (wifi_ble_prov_busy()) {
            continue;
        }

        mqttAttr_t peek;
        cfg_get_mqtt_attr(&peek);
        const int need_stack = mqtt_task_stack_for(peek.transport);
        const bool softap = wifi_softap_is_up();
        bool paused_httpd = false;

        /* Already running — never tear down LAN/SoftAP httpd for a no-op. */
        if (g_MQ.client) {
            continue;
        }

        /*
         * SoftAP only: pause httpd briefly so MQTT can grab a contiguous
         * internal block. STA/LAN must keep the web up — pausing then failing
         * to resume leaves 192.168.x.x dead and looks like a "reboot".
         */
        if (peek.host[0] && softap) {
            int pr = wifi_httpd_pause_for_mqtt();
            if (pr < 0) {
                ESP_LOGW(TAG, "defer start — httpd pause incomplete");
                /* Drop sticky pause claim so SoftAP/LAN httpd can self-heal. */
                wifi_httpd_resume_after_mqtt();
                mqtt_set_fail("httpd pause incomplete");
                if (s_startRetryTimer) {
                    esp_timer_stop(s_startRetryTimer);
                    esp_timer_start_once(s_startRetryTimer, 2000000ULL);
                }
                continue;
            }
            if (pr == 0 && wifi_ble_prov_busy()) {
                continue;
            }
            paused_httpd = (pr == 1);
            for (int i = 0; i < 40 && !internal_heap_ready_for(need_stack); i++) {
                if (wifi_ble_prov_busy()) {
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(50));
            }
        } else if (peek.host[0] && !internal_heap_ready_for(need_stack)) {
            for (int i = 0; i < 30 && !internal_heap_ready_for(need_stack); i++) {
                if (wifi_ble_prov_busy()) {
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(100));
            }
        }

        xSemaphoreTake(g_MQ.mutex, portMAX_DELAY);
        /*
         * BLE enter can win mqtt_stop_wait() while we were coalescing heap
         * outside this lock — re-check before allocating a client.
         */
        if (wifi_ble_prov_busy()) {
            xSemaphoreGive(g_MQ.mutex);
            if (paused_httpd) {
                wifi_httpd_resume_after_mqtt();
            }
            ESP_LOGW(TAG, "skip start — BLE owns radio after heap wait");
            continue;
        }
        if (g_MQ.client) {
            xSemaphoreGive(g_MQ.mutex);
            if (paused_httpd) {
                wifi_httpd_resume_after_mqtt();
            }
            continue;
        }
        size_t free_heap = esp_get_free_heap_size();
        if (free_heap < MQTT_START_MIN_HEAP ||
            (peek.host[0] && !internal_heap_ready_for(need_stack))) {
            ESP_LOGW(TAG, "defer start — free %u largest_int %u need_stack %d softap=%d",
                     (unsigned)free_heap,
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                     need_stack, softap ? 1 : 0);
            mqtt_set_fail("deferred_heap");
            xSemaphoreGive(g_MQ.mutex);
            if (paused_httpd) {
                wifi_httpd_resume_after_mqtt();
            }
            if (s_startRetryTimer) {
                esp_timer_stop(s_startRetryTimer);
                esp_timer_start_once(s_startRetryTimer, 2000000ULL);
            }
            continue;
        }
        if (mqtt_esp_start_locked()) {
            mqtt_clear_fail();
            esp_timer_stop(s_telemetryTimer);
            esp_timer_start_periodic(s_telemetryTimer, TELEMETRY_PERIOD_US);
        } else if (s_startRetryTimer) {
            esp_timer_stop(s_startRetryTimer);
            esp_timer_start_once(s_startRetryTimer, 5000000ULL);
        }
        xSemaphoreGive(g_MQ.mutex);
        if (paused_httpd) {
            wifi_httpd_resume_after_mqtt();
        }
    }
}

static void mqtt_start_retry_cb(void *arg)
{
    (void)arg;
    mqtt_start();
}

void mqtt_start(void)
{
    /* SoftAP XOR BLE: never recreate the client while NimBLE owns the radio. */
    if (wifi_ble_prov_busy()) {
        ESP_LOGW(TAG, "skip start — BLE CT-Prov is up/entering");
        if (s_startRetryTimer) {
            esp_timer_stop(s_startRetryTimer);
        }
        return;
    }
    if (!s_mqttInfraOk || !s_bringupSem) {
        ESP_LOGW(TAG, "mqtt_start skipped — infra not ready");
        return;
    }
    if (g_MQ.client) {
        return;
    }
    if (s_bringupPending) {
        return;
    }
    s_bringupPending = true;
    xSemaphoreGive(s_bringupSem);
}

int mqtt_stop(void)
{
    if (!g_MQ.mutex) {
        return 0; /* mqtt_open not called yet */
    }
    /* Cancel deferred starts before waiting — otherwise a pending
     * mqtt_start_retry can recreate the client during BLE enter. */
    if (s_startRetryTimer) {
        esp_timer_stop(s_startRetryTimer);
    }
    /* Bounded wait — SoftAP↔BLE XOR must not hang on a large telemetry outbox.
     * Do NOT stop the telemetry timer before the lock: a failed take would leave
     * the client running with periodic telemetry permanently disabled. */
    if (xSemaphoreTake(g_MQ.mutex, pdMS_TO_TICKS(1500)) != pdTRUE) {
        ESP_LOGW(TAG, "mqtt_stop: mutex busy — client may still be running");
        return -1;
    }
    if (s_telemetryTimer) {
        esp_timer_stop(s_telemetryTimer);
    }
    g_MQ.isConnected = false; /* prevent further telemetry while tearing down */
    /* Do not publish offline here — QoS1 + preview telemetry can block for seconds. */
    if (g_MQ.client) {
        esp_mqtt_client_stop(g_MQ.client);
        esp_mqtt_client_destroy(g_MQ.client);
        g_MQ.client = NULL;
        g_MQ.started = false;
        ESP_LOGI(TAG, "mqtt stopped");
    }
    rx_reset_unlocked();
    xSemaphoreGive(g_MQ.mutex);
    return 0;
}

int mqtt_stop_wait(int budget_ms)
{
    if (budget_ms < 0) {
        budget_ms = 0;
    }
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(budget_ms);
    int attempt = 0;
    for (;;) {
        if (mqtt_stop() == 0) {
            return 0;
        }
        attempt++;
        TickType_t now = xTaskGetTickCount();
        if ((int32_t)(deadline - now) <= 0) {
            ESP_LOGE(TAG, "mqtt_stop_wait: still busy after %d ms (%d tries)",
                     budget_ms, attempt);
            return -1;
        }
        ESP_LOGW(TAG, "mqtt_stop_wait: retry %d", attempt);
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

int mqtt_restart(void)
{
    if (mqtt_stop_wait(4000) != 0) {
        ESP_LOGE(TAG, "mqtt_restart aborted — stop busy; old client kept");
        return -1;
    }
    mqtt_start();
    return 0;
}

bool mqtt_is_connected(void)
{
    return g_MQ.isConnected;
}

bool mqtt_is_started(void)
{
    return g_MQ.client != NULL;
}

void mqtt_status_detail(char *buf, size_t len)
{
    if (!buf || len == 0) {
        return;
    }
    buf[0] = '\0';
    if (wifi_ble_prov_is_up()) {
        snprintf(buf, len, "ble_paused");
        return;
    }
    if (mqtt_is_connected() && wifi_sta_is_connected()) {
        snprintf(buf, len, "connected");
        return;
    }
    if (!wifi_sta_is_connected()) {
        /* SoftAP alone cannot reach the broker until home Wi‑Fi (APSTA) is up. */
        snprintf(buf, len, wifi_softap_is_up() ? "softap_need_sta" : "no_sta");
        return;
    }
    mqttAttr_t mqtt;
    cfg_get_mqtt_attr(&mqtt);
    if (mqtt.host[0] == '\0') {
        snprintf(buf, len, "no_host");
        return;
    }
    if (!s_mqttInfraOk) {
        snprintf(buf, len, "infra_fail");
        return;
    }
    if (g_MQ.client) {
        snprintf(buf, len, "connecting");
        return;
    }
    /* SoftAP intentionally delayed MQTT so 192.168.4.1 could bind first. */
    if (wifi_softap_mqtt_defer_pending()) {
        snprintf(buf, len, "softap_web_first");
        return;
    }
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    if (largest < mqtt_internal_need(mqtt_task_stack_for(mqtt.transport)) ||
        (s_lastFail[0] && strcmp(s_lastFail, "deferred_heap") == 0)) {
        snprintf(buf, len, "deferred_heap");
        return;
    }
    snprintf(buf, len, "disconnected");
}

int mqtt_test_ping(int timeout_ms, char *errBuf, int errLen)
{
    if (errBuf && errLen > 0) {
        errBuf[0] = '\0';
    }
    if (!wifi_sta_is_connected()) {
        if (errBuf) {
            snprintf(errBuf, errLen,
                     wifi_softap_is_up()
                         ? "need home Wi-Fi (STA) — SoftAP alone cannot reach broker"
                         : "Wi-Fi not connected");
        }
        return -1;
    }
    mqttAttr_t mqtt;
    cfg_get_mqtt_attr(&mqtt);
    if (mqtt.host[0] == '\0') {
        if (errBuf) {
            snprintf(errBuf, errLen, "MQTT host empty");
        }
        return -1;
    }
    /* Already up — session check only. Do NOT publish: telemetry may hold the
     * client lock for a large preview and would wedge the httpd worker. */
    if (g_MQ.isConnected && g_MQ.client) {
        if (errBuf) {
            snprintf(errBuf, errLen, "ok → %s (session up, no publish)", g_MQ.topicUp);
        }
        return 0;
    }

    /* Transport is explicit (mqtt | mqtts) — never infer from port. */
    const bool softap = wifi_softap_is_up();
    mqtt_clear_fail();
    wifi_softap_mqtt_user_start(); /* SoftAP web-first defer must yield to test */
    mqtt_start();
    int waited = 0;
    while (!g_MQ.isConnected && waited < timeout_ms) {
        if (s_connectRefused && s_lastFail[0]) {
            break; /* broker answered — no point waiting out the full budget */
        }
        vTaskDelay(pdMS_TO_TICKS(100));
        waited += 100;
        /* Start may have been deferred for heap — keep trying. */
        if (!g_MQ.client && (waited % 1000) == 0) {
            mqtt_start();
        }
    }
    int rc = 0;
    if (!g_MQ.isConnected || !g_MQ.client) {
        if (errBuf) {
            if (s_lastFail[0]) {
                snprintf(errBuf, errLen, "%s → %s://%s:%d",
                         s_lastFail, mqtt_transport_str(mqtt.transport),
                         mqtt.host, mqtt.port > 0 ? mqtt.port : mqtt_default_port(mqtt.transport));
            } else {
                snprintf(errBuf, errLen, "connect timeout (%d ms) → %s://%s:%d",
                         timeout_ms, mqtt_transport_str(mqtt.transport),
                         mqtt.host, mqtt.port > 0 ? mqtt.port : mqtt_default_port(mqtt.transport));
            }
        }
        rc = -1;
        /* SoftAP + failed probe: stop so TLS/auth retries do not freeze the UI.
         * STA-only: leave client running (same as old firmware). */
        if (softap) {
            (void)mqtt_stop_wait(3000);
        }
    } else {
        /* Leave client up (incl. SoftAP) — bringup already resumed lean httpd. */
        if (errBuf) {
            snprintf(errBuf, errLen, softap
                     ? "ok → %s (%d ms; SoftAP+STA)"
                     : "ok → %s (%d ms)",
                     g_MQ.topicUp, waited);
        }
        (void)softap;
    }
    return rc;
}
