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
#include "app_epd.h"
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
#define MQTT_START_MIN_HEAP 48000
/* Task stacks must live in internal RAM on ESP32-C61 (PSRAM stacks break create). */
#define MQTT_TASK_STACK_TCP 5120
#define MQTT_TASK_STACK_TLS 10240
/* SoftAP APSTA often leaves largest_int ≈6.5KB; keep slack tight so MQTT recovers. */
#define MQTT_TASK_STACK_SLACK 1024

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

static bool internal_heap_ready_for(int stack_bytes)
{
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    return largest >= (size_t)(stack_bytes + MQTT_TASK_STACK_SLACK);
}

static void build_broker_uri(char *uri, size_t uri_len, const mqttAttr_t *mqtt)
{
    int port = mqtt->port > 0 ? mqtt->port : mqtt_default_port(mqtt->transport);
    snprintf(uri, uri_len, "%s://%s:%d", mqtt_transport_str(mqtt->transport), mqtt->host, port);
}

static void publish_raw(const char *topic, const char *json, int qos, int retain)
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

static void publish_up(const char *json, int retain)
{
    if (g_MQ.topicUp[0] == '\0') {
        return;
    }
    mqttAttr_t mqtt;
    cfg_get_mqtt_attr(&mqtt);
    int qos = (mqtt.qos >= 0 && mqtt.qos <= 2) ? mqtt.qos : 1;
    /* Large telemetry (preview) — QoS0 avoids outbox doubling / publish fail. */
    int len = json ? (int)strlen(json) : 0;
    if (len > 16 * 1024) {
        qos = 0;
    }
    publish_raw(g_MQ.topicUp, json, qos, retain);
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
    const char *prev = NULL;
    if (frame.present) {
        cJSON_AddNumberToObject(root, "frame_seq", (double)frame.seq);
        cJSON_AddNumberToObject(root, "frame_crc32", (double)frame.crc32);
        cJSON_AddNumberToObject(root, "frame_saved_us", (double)frame.saved_us);
        cJSON_AddStringToObject(root, "frame_source",
                                frame.source[0] ? frame.source : "");
        cJSON_AddNumberToObject(root, "frame_bytes", (double)LAST_FRAME_BYTES);
        prev = last_frame_preview_b64();
        if (prev && prev[0]) {
            cJSON_AddStringToObject(root, "frame_preview_mime",
                                    last_frame_preview_mime());
            cJSON_AddNumberToObject(root, "frame_preview_w",
                                    last_frame_preview_w());
            cJSON_AddNumberToObject(root, "frame_preview_h",
                                    last_frame_preview_h());
        } else {
            prev = NULL;
        }
    }
    cJSON_AddNumberToObject(root, "hb", 1);
    cJSON_AddBoolToObject(root, "online", true);
    cJSON_AddNumberToObject(root, "ts", (double)now_s());
    ESP_LOGI(TAG, "telemetry build frame_present=%d preview_b64=%u (%dx%d)",
             frame.present ? 1 : 0,
             (unsigned)(prev ? strlen(prev) : 0),
             last_frame_preview_w(), last_frame_preview_h());
    char *head = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!head) {
        ESP_LOGE(TAG, "telemetry JSON print OOM");
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
            return;
        }
        memcpy(out, head, hlen);
        memcpy(out + hlen, prefix, strlen(prefix));
        memcpy(out + hlen + strlen(prefix), prev, plen);
        memcpy(out + hlen + strlen(prefix) + plen, suffix, strlen(suffix) + 1);
        free(head);
        json = out;
    }
    publish_up(json, 0);
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
        publish_up(json, 0);
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
        int rc = app_epd_fill_async(cJSON_IsString(c) ? canvas_color(c->valuestring) : 0x11);
        mqtt_publish_cmd_ack(cmd, rc == 0 ? "ok" : "error",
                           rc == 0 ? "queued" : "render busy");
    } else if (!strcasecmp(cmd, "render_text")) {
        cJSON *t = cJSON_GetObjectItem(p, "text");
        cJSON *c = cJSON_GetObjectItem(p, "color");
        cJSON *bg = cJSON_GetObjectItem(p, "bg");
        cJSON *sc = cJSON_GetObjectItem(p, "scale");
        if (!cJSON_IsString(t)) {
            mqtt_publish_cmd_ack(cmd, "error", "missing text");
        } else {
            espd_render_t render;
            memset(&render, 0, sizeof(render));
            /* Keep full UTF-8 payload — line buffers are 192 B each. */
            size_t tlen = strlen(t->valuestring);
            char *buf = malloc(tlen + 1);
            if (!buf) {
                mqtt_publish_cmd_ack(cmd, "error", "oom");
                cJSON_Delete(root);
                return;
            }
            memcpy(buf, t->valuestring, tlen + 1);
            char *save = NULL;
            for (char *tok = strtok_r(buf, "\n", &save);
                 tok && render.count < EPD_TEXT_MAX_LINES;
                 tok = strtok_r(NULL, "\n", &save)) {
                canvas_utf8_strlcpy(render.lines[render.count++], tok,
                                    sizeof(render.lines[0]));
            }
            free(buf);
            render.fg = cJSON_IsString(c) ? canvas_color(c->valuestring) : 0x00;
            render.bg = cJSON_IsString(bg) ? canvas_color(bg->valuestring) : 0x11;
            render.scale = cJSON_IsNumber(sc) ? sc->valueint : 3;
            int rc = app_epd_render_async(&render);
            mqtt_publish_cmd_ack(cmd, rc == 0 ? "ok" : "error",
                               rc == 0 ? "queued" : "render busy");
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
            int rc = app_epd_canvas_async(cv);
            mqtt_publish_cmd_ack(cmd, rc == 0 ? "ok" : "error",
                               rc == 0 ? "queued" : "render busy");
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
            mqtt_publish_cmd_ack(cmd, rc == 0 ? "ok" : "error",
                               rc == 0 ? "queued" : "render busy");
        }
    } else if (!strcasecmp(cmd, "reboot")) {
        mqtt_publish_cmd_ack(cmd, "ok", "rebooting");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    } else if (!strcasecmp(cmd, "sleep_now") || !strcasecmp(cmd, "sleep")) {
        mqtt_publish_cmd_ack(cmd, "ok", "sleeping");
        vTaskDelay(pdMS_TO_TICKS(400));
        sleep_now();
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
            mqtt_publish_cmd_ack(cmd, "ok", "hibernating");
            vTaskDelay(pdMS_TO_TICKS(400));
            sleep_now();
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

static void enqueue_command(const char *data, int len)
{
    if (len <= 0 || len >= CMD_BUF_SIZE) {
        ESP_LOGW(TAG, "command size %d rejected", len);
        return;
    }
    char *copy = malloc((size_t)len + 1);
    if (!copy) {
        return;
    }
    memcpy(copy, data, (size_t)len);
    copy[len] = 0;
    if (xQueueSend(g_MQ.cmdQueue, &copy, 0) != pdTRUE) {
        mqtt_publish_cmd_ack("(queue)", "error", "device busy");
        free(copy);
    }
}

static void rx_reset(void)
{
    free(g_MQ.rxBuf);
    g_MQ.rxBuf = NULL;
    g_MQ.rxLen = 0;
    g_MQ.rxTotal = 0;
}

static void handle_data_event(esp_mqtt_event_handle_t event)
{
    /* Single-chunk fast path */
    if (event->total_data_len > 0 &&
        event->current_data_offset == 0 &&
        event->data_len == event->total_data_len) {
        enqueue_command(event->data, event->data_len);
        return;
    }

    /* Reassemble fragmented payloads (large render_canvas). */
    if (event->current_data_offset == 0) {
        rx_reset();
        if (event->total_data_len <= 0 || event->total_data_len >= CMD_BUF_SIZE) {
            ESP_LOGW(TAG, "command too large (%d)", event->total_data_len);
            return;
        }
        g_MQ.rxBuf = malloc((size_t)event->total_data_len + 1);
        if (!g_MQ.rxBuf) {
            return;
        }
        g_MQ.rxTotal = event->total_data_len;
        g_MQ.rxLen = 0;
    }
    if (!g_MQ.rxBuf || g_MQ.rxTotal <= 0) {
        return;
    }
    if (event->current_data_offset != g_MQ.rxLen) {
        ESP_LOGW(TAG, "chunk gap %d!=%d — drop", event->current_data_offset, g_MQ.rxLen);
        rx_reset();
        return;
    }
    if (g_MQ.rxLen + event->data_len > g_MQ.rxTotal) {
        rx_reset();
        return;
    }
    memcpy(g_MQ.rxBuf + g_MQ.rxLen, event->data, (size_t)event->data_len);
    g_MQ.rxLen += event->data_len;
    if (g_MQ.rxLen >= g_MQ.rxTotal) {
        enqueue_command(g_MQ.rxBuf, g_MQ.rxTotal);
        rx_reset();
    }
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
            ESP_LOGE(TAG, "subscribe failed (down=%d) — not marking online", sub);
            g_MQ.isConnected = false;
            break;
        }
        g_MQ.isConnected = true;
        publish_status("online", 1);
        mqtt_publish_telemetry();
        break;
    }
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "disconnected");
        g_MQ.isConnected = false;
        rx_reset();
        break;
    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "downlink [%.*s] %d/%d", event->topic_len, event->topic,
                 event->current_data_offset + event->data_len, event->total_data_len);
        handle_data_event(event);
        break;
    case MQTT_EVENT_ERROR:
        if (event->error_handle) {
            ESP_LOGE(TAG, "error type=%d tls=0x%x sock=%d",
                     event->error_handle->error_type,
                     event->error_handle->esp_tls_last_esp_err,
                     event->error_handle->esp_tls_stack_err);
        } else {
            ESP_LOGE(TAG, "error (no detail)");
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

void mqtt_open(void)
{
    memset(&g_MQ, 0, sizeof(mdMqtt_t));
    g_MQ.mutex = xSemaphoreCreateMutex();
    g_MQ.cmdQueue = xQueueCreate(CMD_QUEUE_LEN, sizeof(char *));
    /* cJSON + cmd status publish; EPD queue msgs are heap-backed in app_epd. */
    xTaskCreate(cmd_task, "mqtt_cmd", 8192, NULL, 5, NULL);

    const esp_timer_create_args_t timerArgs = {
        .name = "telemetry", .callback = telemetry_timer_cb,
    };
    ESP_ERROR_CHECK(esp_timer_create(&timerArgs, &s_telemetryTimer));
    const esp_timer_create_args_t retryArgs = {
        .name = "mqtt_start", .callback = mqtt_start_retry_cb,
    };
    ESP_ERROR_CHECK(esp_timer_create(&retryArgs, &s_startRetryTimer));
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

static void mqtt_start_retry_cb(void *arg)
{
    (void)arg;
    mqtt_start();
}

void mqtt_start(void)
{
    xSemaphoreTake(g_MQ.mutex, portMAX_DELAY);
    if (g_MQ.client) {
        xSemaphoreGive(g_MQ.mutex);
        return;
    }
    size_t free_heap = esp_get_free_heap_size();
    if (free_heap < MQTT_START_MIN_HEAP) {
        ESP_LOGW(TAG, "defer start — free heap %u < %u",
                 (unsigned)free_heap, (unsigned)MQTT_START_MIN_HEAP);
        xSemaphoreGive(g_MQ.mutex);
        if (s_startRetryTimer) {
            esp_timer_stop(s_startRetryTimer);
            esp_timer_start_once(s_startRetryTimer, 2000000ULL);
        }
        return;
    }
    if (mqtt_esp_start_locked()) {
        esp_timer_stop(s_telemetryTimer);
        esp_timer_start_periodic(s_telemetryTimer, TELEMETRY_PERIOD_US);
    } else if (s_startRetryTimer) {
        esp_timer_stop(s_startRetryTimer);
        esp_timer_start_once(s_startRetryTimer, 5000000ULL);
    }
    xSemaphoreGive(g_MQ.mutex);
}

void mqtt_stop(void)
{
    if (!g_MQ.mutex) {
        return; /* mqtt_open not called yet (BLE SKU early NimBLE bring-up) */
    }
    xSemaphoreTake(g_MQ.mutex, portMAX_DELAY);
    if (s_telemetryTimer) {
        esp_timer_stop(s_telemetryTimer);
    }
    if (g_MQ.client && g_MQ.isConnected) {
        publish_status("offline", 1);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (g_MQ.client) {
        esp_mqtt_client_stop(g_MQ.client);
        esp_mqtt_client_destroy(g_MQ.client);
        g_MQ.client = NULL;
        g_MQ.isConnected = false;
        g_MQ.started = false;
    }
    rx_reset();
    xSemaphoreGive(g_MQ.mutex);
}

void mqtt_restart(void)
{
    mqtt_stop();
    mqtt_start();
}

bool mqtt_is_connected(void)
{
    return g_MQ.isConnected;
}

int mqtt_test_ping(int timeout_ms, char *errBuf, int errLen)
{
    if (errBuf && errLen > 0) {
        errBuf[0] = '\0';
    }
    if (!wifi_sta_is_connected()) {
        if (errBuf) {
            snprintf(errBuf, errLen, "Wi-Fi not connected");
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
    mqtt_start();
    int waited = 0;
    while (!g_MQ.isConnected && waited < timeout_ms) {
        vTaskDelay(pdMS_TO_TICKS(100));
        waited += 100;
    }
    if (!g_MQ.isConnected || !g_MQ.client) {
        if (errBuf) {
            snprintf(errBuf, errLen, "connect timeout (%d ms) → %s://%s:%d",
                     timeout_ms, mqtt_transport_str(mqtt.transport),
                     mqtt.host, mqtt.port > 0 ? mqtt.port : mqtt_default_port(mqtt.transport));
        }
        return -1;
    }
    mqtt_publish_telemetry();
    if (errBuf) {
        snprintf(errBuf, errLen, "ok → %s (%d ms)", g_MQ.topicUp, waited);
    }
    return 0;
}
