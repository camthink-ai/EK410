/**
 * @file ct_prov.c
 * @brief CT-Prov/1 SoftAP HTTP dispatcher (NeoLink-compatible).
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "evtlog.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "cJSON.h"

#include "sdkconfig.h"
#include "ct_prov.h"
#include "ct_prov_ble.h"
#include "config.h"
#include "wifi.h"
#include "mqtt.h"
#include "version.h"
#define TAG "-->CTPROV"
#define CTPROV_BODY_MAX 4096
#define SCAN_PAGE_SIZE 8

static SemaphoreHandle_t s_busy;

static void ct_ble_close_task(void *arg);
static bool schedule_ble_close(int delay_ms);

static void softap_touch_safe(void)
{
    wifi_softap_touch();
}

void ct_prov_softap_ssid(char *buf, int buflen)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    /* NeoLink legacy SSID: <MODEL>_<MAC6> — see ap_ssid_pattern.dart */
    snprintf(buf, buflen, "EK410_%02X%02X%02X", mac[3], mac[4], mac[5]);
}

void ct_prov_ble_name(char *buf, int buflen)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    /* BLE name charset: A-Z 0-9 - only (hyphen, not underscore). */
    snprintf(buf, buflen, "EK410-%02X%02X%02X", mac[3], mac[4], mac[5]);
}

static esp_err_t send_raw_json(httpd_req_t *req, const char *json)
{
    softap_touch_safe();
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

static char *make_ok(int id, const char *op_result, cJSON *data)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        cJSON_Delete(data);
        return NULL;
    }
    cJSON_AddNumberToObject(root, "id", id);
    cJSON_AddStringToObject(root, "op", op_result);
    cJSON_AddBoolToObject(root, "ok", true);
    if (data) {
        cJSON_AddItemToObject(root, "data", data);
    } else {
        cJSON_AddItemToObject(root, "data", cJSON_CreateObject());
    }
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return s;
}

static char *make_err(int id, const char *op_result, const char *code, const char *message)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *err = cJSON_CreateObject();
    if (!root || !err) {
        cJSON_Delete(root);
        cJSON_Delete(err);
        return NULL;
    }
    cJSON_AddNumberToObject(root, "id", id);
    cJSON_AddStringToObject(root, "op", op_result);
    cJSON_AddBoolToObject(root, "ok", false);
    cJSON_AddStringToObject(err, "code", code);
    cJSON_AddStringToObject(err, "message", message ? message : "");
    cJSON_AddItemToObject(root, "error", err);
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return s;
}

static char *op_device_info(int id)
{
    devInfo_t info;
    cfg_get_dev_info(&info);
    char name[40];
    if (info.devName[0]) {
        strlcpy(name, info.devName, sizeof(name));
    } else if (info.deviceId[0]) {
        snprintf(name, sizeof(name), "EK410-%s", info.deviceId);
    } else {
        strlcpy(name, "EK410", sizeof(name));
    }

    cJSON *data = cJSON_CreateObject();
    cJSON *power = cJSON_CreateArray();
    cJSON_AddItemToArray(power, cJSON_CreateString("battery"));
    cJSON_AddStringToObject(data, "device_name", name);
    cJSON_AddStringToObject(data, "serial_number",
                            info.deviceId[0] ? info.deviceId : info.mac);
    cJSON_AddStringToObject(data, "model", CT_PROV_MODEL_NAME);
    cJSON_AddStringToObject(data, "firmware", "v" EK410_VERSION_STRING);
    cJSON_AddItemToObject(data, "power", power);
    cJSON_AddNumberToObject(data, "protocol_version", 1);
    return make_ok(id, "device.info.get.result", data);
}

static char *op_capabilities(int id)
{
    cJSON *data = cJSON_CreateObject();
    cJSON *sections = cJSON_CreateArray();

    cJSON *wifi = cJSON_CreateObject();
    cJSON *wifi_fields = cJSON_CreateArray();
    cJSON_AddItemToArray(wifi_fields, cJSON_CreateString("ssid"));
    cJSON_AddItemToArray(wifi_fields, cJSON_CreateString("password"));
    cJSON_AddStringToObject(wifi, "name", "wifi");
    cJSON_AddStringToObject(wifi, "kind", "network");
    cJSON_AddBoolToObject(wifi, "configurable", true);
    cJSON_AddBoolToObject(wifi, "scannable", true);
    cJSON_AddItemToObject(wifi, "fields", wifi_fields);
    cJSON_AddItemToArray(sections, wifi);

    cJSON *mqtt = cJSON_CreateObject();
    cJSON *mqtt_fields = cJSON_CreateArray();
    const char *mf[] = {"enable", "host", "port", "username", "password", "tls", "qos"};
    for (size_t i = 0; i < sizeof(mf) / sizeof(mf[0]); i++) {
        cJSON_AddItemToArray(mqtt_fields, cJSON_CreateString(mf[i]));
    }
    cJSON_AddStringToObject(mqtt, "name", "mqtt");
    cJSON_AddStringToObject(mqtt, "kind", "service");
    cJSON_AddBoolToObject(mqtt, "configurable", true);
    cJSON_AddBoolToObject(mqtt, "scannable", false);
    cJSON_AddItemToObject(mqtt, "fields", mqtt_fields);
    cJSON_AddItemToArray(sections, mqtt);

    cJSON *current = cJSON_CreateObject();
    if (config_has_wifi()) {
        cJSON_AddStringToObject(current, "network", "wifi");
    } else {
        cJSON_AddNullToObject(current, "network");
    }

    cJSON_AddItemToObject(data, "sections", sections);
    cJSON_AddItemToObject(data, "current", current);
    return make_ok(id, "communication.capabilities.get.result", data);
}

static char *op_scan(int id, cJSON *req_data)
{
    const char *section = "wifi";
    const char *cursor = "";
    if (cJSON_IsObject(req_data)) {
        cJSON *s = cJSON_GetObjectItem(req_data, "section");
        if (cJSON_IsString(s) && s->valuestring[0]) {
            section = s->valuestring;
        }
        cJSON *c = cJSON_GetObjectItem(req_data, "cursor");
        if (cJSON_IsString(c) && c->valuestring) {
            cursor = c->valuestring;
        }
    }
    if (strcmp(section, "wifi") != 0) {
        return make_err(id, "communication.scan.result", "SECTION_NOT_SUPPORTED",
                        "only wifi scan is supported");
    }

    /* Heap — BLE worker stack is only 4KB; stack APs + cJSON overflowed Rescan. */
    wifi_scan_ap_t *aps = calloc(WIFI_SCAN_MAX, sizeof(*aps));
    if (!aps) {
        return make_err(id, "communication.scan.result", "INTERNAL_ERROR", "oom");
    }
    char err[64] = "";
    int n = wifi_scan(aps, WIFI_SCAN_MAX, err, sizeof(err));
    if (n < 0) {
        free(aps);
        return make_err(id, "communication.scan.result", "INTERNAL_ERROR",
                        err[0] ? err : "scan failed");
    }

    int offset = 0;
    if (cursor[0] && strncmp(cursor, "o", 1) == 0) {
        offset = atoi(cursor + 1);
        if (offset < 0) {
            offset = 0;
        }
    }
    int end = offset + SCAN_PAGE_SIZE;
    if (end > n) {
        end = n;
    }
    bool more = end < n;
    char next_cursor[16] = "";
    if (more) {
        snprintf(next_cursor, sizeof(next_cursor), "o%d", end);
    }

    cJSON *data = cJSON_CreateObject();
    cJSON *items = cJSON_CreateArray();
    for (int i = offset; i < end; i++) {
        cJSON *it = cJSON_CreateObject();
        cJSON_AddStringToObject(it, "network_id", aps[i].ssid);
        cJSON_AddStringToObject(it, "display_name", aps[i].ssid);
        cJSON_AddNumberToObject(it, "signal", aps[i].rssi);
        cJSON_AddItemToArray(items, it);
    }
    free(aps);
    cJSON_AddStringToObject(data, "section", "wifi");
    cJSON_AddItemToObject(data, "items", items);
    cJSON_AddBoolToObject(data, "more", more);
    cJSON_AddStringToObject(data, "cursor", next_cursor);
    return make_ok(id, "communication.scan.result", data);
}

static int apply_wifi_section(cJSON *wifi, char *err, size_t errlen)
{
    if (!cJSON_IsObject(wifi)) {
        snprintf(err, errlen, "wifi must be object");
        return -1;
    }
    cJSON *ssid = cJSON_GetObjectItem(wifi, "ssid");
    cJSON *pass = cJSON_GetObjectItem(wifi, "password");
    cJSON *bssid = cJSON_GetObjectItem(wifi, "bssid");
    if (cJSON_IsString(bssid) && bssid->valuestring[0]) {
        snprintf(err, errlen, "bssid");
        return -2; /* FIELD_NOT_SUPPORTED */
    }
    if (!cJSON_IsString(ssid) || ssid->valuestring[0] == '\0' ||
        strlen(ssid->valuestring) > 31) {
        snprintf(err, errlen, "invalid ssid");
        return -1;
    }
    const char *password = "";
    if (cJSON_IsString(pass) && pass->valuestring) {
        password = pass->valuestring;
        size_t plen = strlen(password);
        if (plen != 0 && (plen < 8 || plen > 63)) {
            snprintf(err, errlen, "invalid password length");
            return -1;
        }
    }

    wifiAttr_t w = {0};
    strlcpy(w.ssid, ssid->valuestring, sizeof(w.ssid));
    strlcpy(w.password, password, sizeof(w.password));

#if CONFIG_BT_NIMBLE_ENABLED
    /*
     * During BLE window, WPA associate is best-effort (C61 coex with NimBLE).
     * Always persist credentials; STA reconnects after BLE exit if needed.
     * Return 1 → caller schedules BLE teardown (NeoLink often skips session.close).
     */
    if (wifi_ble_prov_is_up() || ct_prov_ble_is_up()) {
        cfg_set_wifi_attr(&w);
        ESP_LOGI(TAG, "BLE window: saved wifi ssid=%s", w.ssid);
        if (wifi_sta_try_connect(w.ssid, w.password, 6000)) {
            ESP_LOGI(TAG, "BLE window: early associate ok");
            evtlog_event("CTPROV", "wifi saved ssid=%s early_ok=1", w.ssid);
        } else {
            ESP_LOGW(TAG, "BLE window: associate deferred until BLE exit");
            evtlog_event("CTPROV", "wifi saved ssid=%s early_ok=0", w.ssid);
        }
        return 1;
    }
#endif

    bool ok = wifi_sta_try_connect(ssid->valuestring, password, 20000);
    if (!ok) {
        snprintf(err, errlen, "wifi associate failed");
        return -3;
    }
    cfg_set_wifi_attr(&w);
    return 0;
}

static int apply_mqtt_section(cJSON *mqtt, char *err, size_t errlen)
{
    if (!cJSON_IsObject(mqtt)) {
        snprintf(err, errlen, "mqtt must be object");
        return -1;
    }
    cJSON *enable = cJSON_GetObjectItem(mqtt, "enable");
    if (cJSON_IsBool(enable) && !cJSON_IsTrue(enable)) {
        /* Stop first — only clear host when the client is actually gone. */
        if (mqtt_stop_wait(4000) != 0) {
            snprintf(err, errlen, "mqtt stop busy");
            return -4;
        }
        mqttAttr_t m;
        cfg_get_mqtt_attr(&m);
        m.host[0] = '\0';
        if (cfg_set_mqtt_attr(&m) != ESP_OK) {
            snprintf(err, errlen, "mqtt config save failed");
            return -3;
        }
        return 0;
    }

    cJSON *host = cJSON_GetObjectItem(mqtt, "host");
    cJSON *port = cJSON_GetObjectItem(mqtt, "port");
    cJSON *user = cJSON_GetObjectItem(mqtt, "username");
    cJSON *pass = cJSON_GetObjectItem(mqtt, "password");
    cJSON *tls = cJSON_GetObjectItem(mqtt, "tls");
    cJSON *qos = cJSON_GetObjectItem(mqtt, "qos");
    /* NeoLink may send topic / client_id / clientId — device-bound, ignore. */
    (void)cJSON_GetObjectItem(mqtt, "topic");
    (void)cJSON_GetObjectItem(mqtt, "clientId");
    (void)cJSON_GetObjectItem(mqtt, "client_id");
    if (!cJSON_IsString(host) || host->valuestring[0] == '\0' ||
        strlen(host->valuestring) >= 64) {
        snprintf(err, errlen, "invalid host");
        return -1;
    }

    mqttAttr_t m;
    cfg_get_mqtt_attr(&m);
    strlcpy(m.host, host->valuestring, sizeof(m.host));
    bool use_tls = cJSON_IsTrue(tls);
    m.transport = use_tls ? MQTT_TRANSPORT_TLS : MQTT_TRANSPORT_TCP;
    if (cJSON_IsNumber(port) && port->valueint > 0 && port->valueint < 65536) {
        m.port = port->valueint;
    } else {
        m.port = mqtt_default_port(m.transport);
    }
    if (cJSON_IsNumber(qos) && qos->valueint >= 0 && qos->valueint <= 2) {
        m.qos = qos->valueint;
    }
    if (cJSON_IsString(user)) {
        strlcpy(m.username, user->valuestring, sizeof(m.username));
    }
    if (cJSON_IsString(pass)) {
        strlcpy(m.password, pass->valuestring, sizeof(m.password));
    }
    {
        devInfo_t info;
        cfg_get_dev_info(&info);
        snprintf(m.clientId, sizeof(m.clientId), "ek410-%s", info.deviceId);
    }
    if (cJSON_IsBool(tls)) {
        m.tlsVerify = true; /* NeoLink tls flag selects transport; keep verify on */
    }
    /* Same order as enable:false — stop first, then persist, then start.
     * Busy stop keeps old broker in NVS and on the wire. */
    if (wifi_sta_is_connected()) {
        if (mqtt_stop_wait(4000) != 0) {
            snprintf(err, errlen, "mqtt stop busy");
            return -4;
        }
    }
    if (cfg_set_mqtt_attr(&m) != ESP_OK) {
        snprintf(err, errlen, "mqtt config save failed");
        return -3;
    }
    if (wifi_sta_is_connected()) {
        mqtt_start();
    }
    return 0;
}

static char *op_config_set(int id, cJSON *req_data)
{
    if (!cJSON_IsObject(req_data)) {
        return make_err(id, "config.set.result", "INVALID_ARGUMENT", "data must be object");
    }

    cJSON *stored = cJSON_CreateArray();
    char err[64] = "";
    const char *apply = "immediate";
    bool ble_wifi_saved = false;

    cJSON *wifi = cJSON_GetObjectItem(req_data, "wifi");
    if (wifi) {
        int rc = apply_wifi_section(wifi, err, sizeof(err));
        if (rc == -2) {
            cJSON_Delete(stored);
            return make_err(id, "config.set.result", "FIELD_NOT_SUPPORTED", err);
        }
        if (rc < 0) {
            cJSON_Delete(stored);
            return make_err(id, "config.set.result",
                            rc == -3 ? "STORAGE_FAILED" : "INVALID_ARGUMENT", err);
        }
        if (rc == 1) {
            ble_wifi_saved = true;
        }
        cJSON_AddItemToArray(stored, cJSON_CreateString("wifi"));
        apply = "reconnect";
    }

    cJSON *mqtt = cJSON_GetObjectItem(req_data, "mqtt");
    if (mqtt) {
        int rc = apply_mqtt_section(mqtt, err, sizeof(err));
        if (rc == -2) {
            cJSON_Delete(stored);
            return make_err(id, "config.set.result", "FIELD_NOT_SUPPORTED", err);
        }
        if (rc != 0) {
            cJSON_Delete(stored);
            const char *code = "INVALID_ARGUMENT";
            if (rc == -4) {
                code = "RESOURCE_BUSY";
            } else if (rc == -3) {
                code = "STORAGE_FAILED";
            }
            return make_err(id, "config.set.result", code, err);
        }
        cJSON_AddItemToArray(stored, cJSON_CreateString("mqtt"));
    }

    /* Reject unknown top-level sections that are not empty. */
    cJSON *child = req_data->child;
    while (child) {
        if (strcmp(child->string, "wifi") != 0 && strcmp(child->string, "mqtt") != 0) {
            cJSON_Delete(stored);
            return make_err(id, "config.set.result", "SECTION_NOT_SUPPORTED", child->string);
        }
        child = child->next;
    }

    if (cJSON_GetArraySize(stored) == 0) {
        cJSON_Delete(stored);
        return make_err(id, "config.set.result", "INVALID_ARGUMENT", "no sections");
    }

    cJSON *data = cJSON_CreateObject();
    cJSON_AddItemToObject(data, "stored", stored);
    cJSON_AddStringToObject(data, "apply", apply);

    /* BLE: leave the cyan window after Wi-Fi is saved — otherwise the LED
     * keeps blinking until idle (2 min) / session.close, looking "stuck blue".
     * Delay long enough for the GATT indicate of this result to finish. */
    if (ble_wifi_saved && wifi_ble_prov_is_up()) {
        ESP_LOGI(TAG, "BLE wifi saved — schedule CT-Prov exit → STA join");
        if (!schedule_ble_close(1200)) {
            ESP_LOGW(TAG, "BLE exit task create failed — idle will close");
        }
    }

    return make_ok(id, "config.set.result", data);
}

static char *op_config_get(int id, cJSON *req_data)
{
    (void)req_data;
    wifiAttr_t w;
    mqttAttr_t m;
    cfg_get_wifi_attr(&w);
    cfg_get_mqtt_attr(&m);

    cJSON *data = cJSON_CreateObject();
    if (w.ssid[0]) {
        cJSON *wifi = cJSON_CreateObject();
        cJSON_AddStringToObject(wifi, "ssid", w.ssid);
        /* Password is write-only; omit on get. */
        cJSON_AddItemToObject(data, "wifi", wifi);
    }
    if (m.host[0]) {
        cJSON *mqtt = cJSON_CreateObject();
        cJSON_AddBoolToObject(mqtt, "enable", true);
        cJSON_AddStringToObject(mqtt, "host", m.host);
        cJSON_AddNumberToObject(mqtt, "port", m.port);
        cJSON_AddBoolToObject(mqtt, "tls", m.transport == MQTT_TRANSPORT_TLS);
        cJSON_AddNumberToObject(mqtt, "qos", m.qos);
        if (m.username[0]) {
            cJSON_AddStringToObject(mqtt, "username", m.username);
        }
        cJSON_AddItemToObject(data, "mqtt", mqtt);
    }
    return make_ok(id, "config.get.result", data);
}

static void ct_softap_close_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(600));
    wifi_softap_close();
    vTaskDelete(NULL);
}

static void ct_ble_close_task(void *arg)
{
    int delay_ms = arg ? (int)(intptr_t)arg : 600;
    if (delay_ms < 200) {
        delay_ms = 200;
    }
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
    wifi_ble_prov_close();
    vTaskDelete(NULL);
}

static bool schedule_ble_close(int delay_ms)
{
    if (!wifi_ble_prov_is_up()) {
        return false;
    }
    void *arg = (void *)(intptr_t)delay_ms;
    if (xTaskCreate(ct_ble_close_task, "ct_ble_off", 4096, arg, 5, NULL) != pdPASS) {
        return false;
    }
    return true;
}

static char *op_session_close(int id)
{
    /* Ack first; tear down SoftAP or BLE window after the response is out. */
    if (wifi_ble_prov_is_up()) {
        if (!schedule_ble_close(600)) {
            wifi_ble_prov_close();
        }
    } else if (wifi_softap_is_up()) {
        if (xTaskCreate(ct_softap_close_task, "ct_ap_off", 3072, NULL, 5, NULL) != pdPASS) {
            wifi_softap_close();
        }
    }
    return make_ok(id, "session.close.result", NULL);
}

static char *dispatch(int id, const char *op, cJSON *data)
{
    if (strcmp(op, "device.info.get") == 0) {
        return op_device_info(id);
    }
    if (strcmp(op, "communication.capabilities.get") == 0) {
        return op_capabilities(id);
    }
    if (strcmp(op, "communication.scan") == 0) {
        return op_scan(id, data);
    }
    if (strcmp(op, "config.set") == 0) {
        return op_config_set(id, data);
    }
    if (strcmp(op, "config.get") == 0) {
        return op_config_get(id, data);
    }
    if (strcmp(op, "session.close") == 0) {
        return op_session_close(id);
    }
    char result_op[96];
    snprintf(result_op, sizeof(result_op), "%s.result", op);
    return make_err(id, result_op, "UNSUPPORTED_OPERATION", op);
}

esp_err_t ct_prov_handle_json(const char *body, int len, char **out_json)
{
    if (out_json) {
        *out_json = NULL;
    }
    if (!body || len <= 0 || len > CTPROV_BODY_MAX || !out_json) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Reject UTF-8 BOM (NeoLink envelope rules). */
    if (len >= 3 && (uint8_t)body[0] == 0xEF && (uint8_t)body[1] == 0xBB &&
        (uint8_t)body[2] == 0xBF) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_busy) {
        s_busy = xSemaphoreCreateMutex();
    }
    if (!s_busy) {
        return ESP_ERR_NO_MEM;
    }
    if (xSemaphoreTake(s_busy, 0) != pdTRUE) {
        *out_json = make_err(0, "protocol.error.result", "BUSY",
                             "another request in flight");
        return ESP_ERR_INVALID_STATE;
    }

    softap_touch_safe();

    cJSON *root = cJSON_ParseWithLength(body, (size_t)len);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        xSemaphoreGive(s_busy);
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *jid = cJSON_GetObjectItem(root, "id");
    cJSON *jop = cJSON_GetObjectItem(root, "op");
    cJSON *jdata = cJSON_GetObjectItem(root, "data");
    if (!cJSON_IsNumber(jid) || jid->valuedouble != (double)(int)jid->valuedouble ||
        jid->valueint < 0 || !cJSON_IsString(jop) || jop->valuestring[0] == '\0' ||
        !cJSON_IsObject(jdata)) {
        cJSON_Delete(root);
        xSemaphoreGive(s_busy);
        return ESP_ERR_INVALID_ARG;
    }

    int id = jid->valueint;
    const char *op = jop->valuestring;
    char *resp = dispatch(id, op, jdata);
    cJSON_Delete(root);
    xSemaphoreGive(s_busy);

    if (!resp) {
        return ESP_ERR_NO_MEM;
    }
    *out_json = resp;
    return ESP_OK;
}

esp_err_t ct_prov_http_handle(httpd_req_t *req)
{
    softap_touch_safe();

    int total = req->content_len;
    if (total <= 0 || total > CTPROV_BODY_MAX) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_send(req, NULL, 0);
    }

    char *body = malloc((size_t)total + 1);
    if (!body) {
        return ESP_FAIL;
    }
    int received = 0;
    while (received < total) {
        int got = httpd_req_recv(req, body + received, total - received);
        if (got <= 0) {
            free(body);
            return ESP_FAIL;
        }
        received += got;
    }
    body[total] = '\0';

    char *resp = NULL;
    esp_err_t hr = ct_prov_handle_json(body, total, &resp);
    free(body);

    if (hr == ESP_ERR_INVALID_ARG) {
        free(resp);
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_send(req, NULL, 0);
    }
    if (hr == ESP_ERR_INVALID_STATE) {
        httpd_resp_set_status(req, "400 Bad Request");
        if (!resp) {
            return send_raw_json(req, "{\"id\":0,\"op\":\"error.result\",\"ok\":false,"
                                      "\"error\":{\"code\":\"BUSY\",\"message\":\"busy\"}}");
        }
        esp_err_t r = send_raw_json(req, resp);
        free(resp);
        return r;
    }
    if (hr != ESP_OK || !resp) {
        free(resp);
        return ESP_FAIL;
    }
    esp_err_t r = send_raw_json(req, resp);
    free(resp);
    return r;
}
