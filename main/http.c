/**
 * @file http.c
 * @brief Web config UI + REST API, CamThink /api/v1 conventions.
 *
 * GET  /                          embedded single-page config UI
 * GET  /api/v1/system/getDevInfo
 * POST /api/v1/system/setDevInfo          {devName}
 * POST /api/v1/system/setDevReset
 * POST /api/v1/system/setDevFactory
 * GET/POST /api/v1/network/getWifiParam | setWifiParam {ssid,password}
 * GET  /api/v1/network/scanWifi           {aps:[{ssid,rssi,secure,authmode}]}
 * GET/POST /api/v1/network/getMqttParam | setMqttParam
 *        {host,port,transport:"mqtt"|"mqtts",clientId,qos,username,password,
 *         tlsVerify,caPem}
 * POST /api/v1/display/renderText         {text,color,bg,scale}
 * POST /api/v1/display/testPattern
 * POST /api/v1/network/testMqtt           probe connect + publish ping
 * POST /api/v1/network/closeSoftAp        exit config / close SoftAP
 * POST /api/v1/display/renderCanvas       {bg,items[]}
 * POST /api/v1/display/displayUrl         {url}  device-side fetch + decode
 * POST /api/v1/display/proxyImage         {url}  STA fetch → raw image bytes (SoftAP preview)
 * GET  /api/v1/display/getLastFrameMeta   {present,seq,source,savedUs,bytes,crc32}
 * GET  /api/v1/display/lastFrame          raw 960000-byte native 4bpp frame
 * GET/POST /api/v1/system/getPowerParam | setPowerParam
 *        {mode:always|cycle|hibernate, sleepMin, wakeSec, wakeUrl,
 *         imageQuality:"quality"|"speed"}
 * POST /api/v1/system/sleepNow            deep sleep (MAIN wakes; cycle also timer)
 * GET  /api/v1/system/getEventLogMeta     {present,bytes,seq,capBytes,keepDays,timeOk}
 * GET  /api/v1/system/eventLog            text/plain download (critical events)
 * POST /api/v1/system/clearEventLog
 * POST /api/v1/system/setDevUpgrade          {url:"https://…"} HTTPS OTA
 * POST /api/v1/system/uploadFirmware         raw application/octet-stream .bin
 *
 * Handlers return the data JSON directly (no envelope), like lowpower_camera.
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "cJSON.h"

#include "http.h"
#include "json_guard.h"
#include "config.h"
#include "wifi.h"
#include "mqtt.h"
#include "app_epd.h"
#include "canvas.h"
#include "battery.h"
#include "ota.h"
#include "sleep.h"
#include "led.h"
#include "last_frame.h"
#include "evtlog.h"
#include "ct_prov.h"
#include "www_html.h"

#define TAG "-->HTTP"

static void softap_touch(void)
{
    wifi_softap_touch();
}

static esp_err_t send_json(httpd_req_t *req, const char *str)
{
    softap_touch();
    /* cJSON_Print returns NULL on OOM — never strlen(NULL). */
    if (!str) {
        str = "{\"code\":1,\"msg\":\"out of memory\"}";
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, str);
}

static esp_err_t send_err(httpd_req_t *req, const char *msg)
{
    char buf[96];
    snprintf(buf, sizeof(buf), "{\"code\":1,\"msg\":\"%s\"}", msg);
    return send_json(req, buf);
}

static char *read_body(httpd_req_t *req);

/* Smallest free stack ever seen (bytes) for the tasks this firmware runs on
 * its tightest budgets. Looked up by name through xTaskGetHandle(), so no
 * task-list buffer is needed on the httpd stack. */
static void add_stack_min(cJSON *obj)
{
    static const char *const kTasks[] = {
        "httpd",     "mqtt_task", "mqtt_cmd", "mqtt_up",  "sys_evt",
        "esp_timer", "epd_render", "button", "ct_ble",   "wifi",
        "nimble_host", "led",
    };
    cJSON *sm = cJSON_AddObjectToObject(obj, "stackMin");
    if (!sm) {
        return;
    }
    for (size_t i = 0; i < sizeof(kTasks) / sizeof(kTasks[0]); i++) {
        TaskHandle_t h = xTaskGetHandle(kTasks[i]);
        if (h) {
            cJSON_AddNumberToObject(sm, kTasks[i], uxTaskGetStackHighWaterMark(h));
        }
    }
}

static char *read_body(httpd_req_t *req)
{
    softap_touch();
    int total = req->content_len;
    /* Canvas JSON with many layers can exceed 4KB. */
    if (total <= 0 || total > 16384) {
        return NULL;
    }
    char *body = malloc(total + 1);
    if (!body) {
        return NULL;
    }
    int received = 0;
    while (received < total) {
        int got = httpd_req_recv(req, body + received, total - received);
        if (got <= 0) {
            free(body);
            return NULL;
        }
        received += got;
    }
    body[total] = 0;
    return body;
}

/* ---------- system ---------- */

static esp_err_t get_dev_info_handle(httpd_req_t *req)
{
    devInfo_t info;
    wifiAttr_t wifi;
    mqttAttr_t mqtt;
    char ip[20] = "";
    cfg_get_dev_info(&info);
    cfg_get_wifi_attr(&wifi);
    cfg_get_mqtt_attr(&mqtt);
    wifi_get_ip_str(ip, sizeof(ip));

    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "devName", info.devName);
    cJSON_AddStringToObject(obj, "devMac", info.mac);
    cJSON_AddStringToObject(obj, "deviceId", info.deviceId);
    {
        char apSsid[33] = {0};
        wifi_get_ap_ssid(apSsid, sizeof(apSsid));
        cJSON_AddStringToObject(obj, "softApSsid", apSsid);
    }
    cJSON_AddStringToObject(obj, "fwVersion", info.fwVersion);
    cJSON_AddStringToObject(obj, "panel", info.panel);
    cJSON_AddStringToObject(obj, "ip", ip);
    cJSON_AddBoolToObject(obj, "wifiConnected", wifi_sta_is_connected());
    cJSON_AddStringToObject(obj, "wifiState", wifi_sta_state_str());
    {
        wifiAttr_t w;
        cfg_get_wifi_attr(&w);
        cJSON_AddStringToObject(obj, "wifiSsid", w.ssid);
    }
    cJSON_AddBoolToObject(obj, "mqttConnected",
                          mqtt_is_connected() && wifi_sta_is_connected());
    cJSON_AddBoolToObject(obj, "mqttStarted", mqtt_is_started());
    cJSON_AddBoolToObject(obj, "mqttInfraOk", mqtt_infra_ok());
    {
        char mqReason[32];
        mqtt_status_detail(mqReason, sizeof(mqReason));
        cJSON_AddStringToObject(obj, "mqttReason", mqReason);
    }
    {
        const char *le = mqtt_last_error();
        if (le && le[0]) {
            cJSON_AddStringToObject(obj, "mqttLastError", le);
        }
    }
    cJSON_AddBoolToObject(obj, "httpServerUp", http_server_is_up());
    cJSON_AddBoolToObject(obj, "softapUp", wifi_softap_is_up());
    cJSON_AddBoolToObject(obj, "bleProvUp", wifi_ble_prov_is_up());
    /* Single image always includes NimBLE CT-Prov (XOR with SoftAP). */
    cJSON_AddBoolToObject(obj, "bleSkuAvailable", true);
    {
        const char *pe = wifi_last_error();
        if (pe && pe[0]) {
            cJSON_AddStringToObject(obj, "provError", pe);
        }
    }
    cJSON_AddStringToObject(obj, "projectName", ota_running_project_name());
    cJSON_AddBoolToObject(obj, "provisioned", config_is_provisioned());
    cJSON_AddBoolToObject(obj, "panelOk", app_epd_panel_ok());
    cJSON_AddBoolToObject(obj, "displayBusy", app_epd_display_busy());
    cJSON_AddBoolToObject(obj, "lastDisplayOk", app_epd_last_display_ok());
    {
        evtlog_meta_t em;
        evtlog_get_meta(&em);
        cJSON_AddBoolToObject(obj, "evtLogPresent", em.present);
        cJSON_AddNumberToObject(obj, "evtLogBytes", em.bytes);
    }
    if (wifi_softap_is_up()) {
        cJSON_AddStringToObject(obj, "softApPass", "12345678");
    }
    cJSON_AddNumberToObject(obj, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    int batMv = battery_read_mv();
    cJSON_AddBoolToObject(obj, "batterySense", batMv >= 0);
    if (batMv >= 0) {
        cJSON_AddNumberToObject(obj, "batteryMv", batMv);
        cJSON_AddNumberToObject(obj, "batteryPercent", battery_soc_from_mv(batMv));
    }
    char running[16];
    bool pendingRollback = false;
    ota_get_status(running, sizeof(running), &pendingRollback);
    cJSON_AddStringToObject(obj, "otaPartition", running);
    cJSON_AddBoolToObject(obj, "otaPendingVerify", pendingRollback);
    /* A URL OTA POST returns before its download: the web UI polls these. */
    cJSON_AddStringToObject(obj, "otaState", ota_state_str());
    if (ota_last_error()[0]) {
        cJSON_AddStringToObject(obj, "otaError", ota_last_error());
    }
    powerAttr_t p;
    cfg_get_power_attr(&p);
    cJSON_AddStringToObject(obj, "powerMode", power_mode_str(p.mode));
    cJSON_AddNumberToObject(obj, "sleepMin", p.sleepMin);
    cJSON_AddNumberToObject(obj, "wakeSec", p.wakeSec);
    char *str = cJSON_PrintUnformatted(obj);
    esp_err_t res = send_json(req, str);
    cJSON_free(str);
    cJSON_Delete(obj);
    return res;
}

/* Field diagnostics, polled on demand: heap headroom and, per long-lived
 * task, the smallest amount of stack ever left. Deliberately NOT part of
 * getDevInfo — the web UI polls that every few seconds. */
static esp_err_t get_diag_handle(httpd_req_t *req)
{
    cJSON *obj = cJSON_CreateObject();
    if (!obj) {
        return send_err(req, "oom");
    }
    cJSON *heap = cJSON_AddObjectToObject(obj, "heap");
    if (heap) {
        cJSON_AddNumberToObject(heap, "free", esp_get_free_heap_size());
        cJSON_AddNumberToObject(heap, "minFree", esp_get_minimum_free_heap_size());
        cJSON_AddNumberToObject(heap, "freeInt",
                                 heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        cJSON_AddNumberToObject(heap, "minFreeInt",
                                 heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
        cJSON_AddNumberToObject(heap, "largestInt",
                                 heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        cJSON_AddNumberToObject(heap, "freeSpiram",
                                 heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        cJSON_AddNumberToObject(heap, "largestSpiram",
                                 heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    }
    add_stack_min(obj);
    char *str = cJSON_PrintUnformatted(obj);
    esp_err_t res = send_json(req, str);
    cJSON_free(str);
    cJSON_Delete(obj);
    return res;
}

static esp_err_t set_dev_info_handle(httpd_req_t *req)
{
    char *body = read_body(req);
    if (!body) {
        return send_err(req, "bad body");
    }
    cJSON *root = json_guard_parse(body);
    free(body);
    cJSON *name = root ? cJSON_GetObjectItem(root, "devName") : NULL;
    if (!cJSON_IsString(name) || name->valuestring[0] == '\0') {
        cJSON_Delete(root);
        return send_err(req, "missing devName");
    }
    /* Stored in a 33-byte field: reject instead of saving an unreadable name. */
    esp_err_t err = cfg_set_dev_name(name->valuestring);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        return send_err(req, err == ESP_ERR_INVALID_ARG ? "devName too long (max 32 bytes)"
                                                        : "devName save failed");
    }
    return send_json(req, "{\"code\":0,\"msg\":\"ok\"}");
}

static esp_err_t set_dev_reset_handle(httpd_req_t *req)
{
    send_json(req, "{\"code\":0,\"msg\":\"rebooting\"}");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK; /* unreachable */
}

static esp_err_t set_dev_factory_handle(httpd_req_t *req)
{
    esp_err_t err = cfg_factory_reset();
    if (err != ESP_OK) {
        return send_err(req, "factory reset failed");
    }
    send_json(req, "{\"code\":0,\"msg\":\"factory reset, rebooting\"}");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK; /* unreachable */
}

/* ---------- network ---------- */

static esp_err_t get_wifi_param_handle(httpd_req_t *req)
{
    wifiAttr_t wifi;
    char ip[16] = { 0 };
    cfg_get_wifi_attr(&wifi);
    wifi_get_ip_str(ip, sizeof(ip));
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "ssid", wifi.ssid);
    cJSON_AddBoolToObject(obj, "hasPassword", wifi.password[0] != '\0');
    cJSON_AddBoolToObject(obj, "isConnected", wifi_sta_is_connected());
    cJSON_AddStringToObject(obj, "state", wifi_sta_state_str());
    cJSON_AddStringToObject(obj, "ip", ip);
    char *str = cJSON_PrintUnformatted(obj);
    esp_err_t res = send_json(req, str);
    cJSON_free(str);
    cJSON_Delete(obj);
    return res;
}

static esp_err_t scan_wifi_handle(httpd_req_t *req)
{
    softap_touch();
    wifi_scan_ap_t aps[WIFI_SCAN_MAX];
    char err[64] = "";
    int n = wifi_scan(aps, WIFI_SCAN_MAX, err, sizeof(err));
    if (n < 0) {
        return send_err(req, err[0] ? err : "scan failed");
    }
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(obj, "code", 0);
    cJSON *arr = cJSON_AddArrayToObject(obj, "aps");
    for (int i = 0; i < n; i++) {
        cJSON *ap = cJSON_CreateObject();
        cJSON_AddStringToObject(ap, "ssid", aps[i].ssid);
        cJSON_AddNumberToObject(ap, "rssi", aps[i].rssi);
        cJSON_AddBoolToObject(ap, "secure", aps[i].secure);
        cJSON_AddNumberToObject(ap, "authmode", aps[i].authmode);
        cJSON_AddItemToArray(arr, ap);
    }
    cJSON_AddNumberToObject(obj, "count", n);
    char *str = cJSON_PrintUnformatted(obj);
    esp_err_t res = send_json(req, str);
    cJSON_free(str);
    cJSON_Delete(obj);
    return res;
}

static esp_err_t set_wifi_param_handle(httpd_req_t *req)
{
    char *body = read_body(req);
    if (!body) {
        return send_err(req, "bad body");
    }
    cJSON *root = json_guard_parse(body);
    free(body);
    cJSON *ssid = root ? cJSON_GetObjectItem(root, "ssid") : NULL;
    cJSON *pass = root ? cJSON_GetObjectItem(root, "password") : NULL;
    if (!cJSON_IsString(ssid) || ssid->valuestring[0] == '\0') {
        cJSON_Delete(root);
        return send_err(req, "missing ssid");
    }

    wifiAttr_t candidate;
    memset(&candidate, 0, sizeof(candidate));
    strlcpy(candidate.ssid, ssid->valuestring, sizeof(candidate.ssid));
    if (cJSON_IsString(pass) && pass->valuestring[0] != '\0') {
        strlcpy(candidate.password, pass->valuestring, sizeof(candidate.password));
    } else {
        /* Empty password → reuse NVS secret when SSID matches. */
        wifiAttr_t saved;
        cfg_get_wifi_attr(&saved);
        if (strcmp(saved.ssid, candidate.ssid) == 0 && saved.password[0] != '\0') {
            strlcpy(candidate.password, saved.password, sizeof(candidate.password));
        }
    }
    cJSON_Delete(root);

    wifi_softap_touch();
    bool ok = wifi_sta_try_connect(candidate.ssid, candidate.password, 20000);
    if (!ok) {
        /* try_connect already restored saved network or cleared unsaved STA. */
        return send_err(req, "connect failed");
    }

    cfg_set_wifi_attr(&candidate);

    char ip[16] = { 0 };
    wifi_get_ip_str(ip, sizeof(ip));
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(obj, "code", 0);
    cJSON_AddBoolToObject(obj, "connected", true);
    cJSON_AddStringToObject(obj, "ssid", candidate.ssid);
    cJSON_AddStringToObject(obj, "ip", ip);
    cJSON_AddStringToObject(obj, "state", wifi_sta_state_str());
    cJSON_AddStringToObject(obj, "msg", "connected");
    char *str = cJSON_PrintUnformatted(obj);
    esp_err_t res = send_json(req, str);
    cJSON_free(str);
    cJSON_Delete(obj);
    return res;
}

static esp_err_t get_mqtt_param_handle(httpd_req_t *req)
{
    mqttAttr_t mqtt;
    cfg_get_mqtt_attr(&mqtt);
    devInfo_t info;
    cfg_get_dev_info(&info);
    char topicUp[96], topicDown[96];
    const char *did = info.deviceId[0] ? info.deviceId : "unknown";
    snprintf(topicUp, sizeof(topicUp), "device/ek410_Eink/%s/uplink", did);
    snprintf(topicDown, sizeof(topicDown), "device/ek410_Eink/%s/downlink", did);

    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "host", mqtt.host);
    cJSON_AddNumberToObject(obj, "port", mqtt.port);
    cJSON_AddStringToObject(obj, "transport", mqtt_transport_str(mqtt.transport));
    cJSON_AddBoolToObject(obj, "tls", mqtt.transport == MQTT_TRANSPORT_TLS);
    cJSON_AddBoolToObject(obj, "tlsVerify", mqtt.tlsVerify);
    cJSON_AddStringToObject(obj, "caPem", cfg_mqtt_ca_pem());
    cJSON_AddStringToObject(obj, "clientId", mqtt.clientId);
    cJSON_AddNumberToObject(obj, "qos", mqtt.qos);
    cJSON_AddStringToObject(obj, "username", mqtt.username);
    /* Secret is write-only like the Wi-Fi password: a blank field on save
     * means "unchanged" (see setMqttParam). */
    cJSON_AddBoolToObject(obj, "hasPassword", mqtt.password[0] != '\0');
    cJSON_AddStringToObject(obj, "topicUp", topicUp);
    cJSON_AddStringToObject(obj, "topicDown", topicDown);
    cJSON_AddNumberToObject(obj, "keepalive_s", 30);
    cJSON_AddNumberToObject(obj, "heartbeat_s", 30);
    /* Match getDevInfo: require STA so LAN UI cannot show MQTT up while Wi‑Fi is down. */
    cJSON_AddBoolToObject(obj, "connected",
                          mqtt_is_connected() && wifi_sta_is_connected());
    char *str = cJSON_PrintUnformatted(obj);
    esp_err_t res = send_json(req, str);
    cJSON_free(str);
    cJSON_Delete(obj);
    return res;
}

static esp_err_t set_mqtt_param_handle(httpd_req_t *req)
{
    char *body = read_body(req);
    if (!body) {
        return send_err(req, "bad body");
    }
    cJSON *root = json_guard_parse(body);
    free(body);
    cJSON *host = root ? cJSON_GetObjectItem(root, "host") : NULL;
    cJSON *port = root ? cJSON_GetObjectItem(root, "port") : NULL;
    cJSON *transport = root ? cJSON_GetObjectItem(root, "transport") : NULL;
    cJSON *tls = root ? cJSON_GetObjectItem(root, "tls") : NULL;
    cJSON *cid = root ? cJSON_GetObjectItem(root, "clientId") : NULL;
    cJSON *qos = root ? cJSON_GetObjectItem(root, "qos") : NULL;
    cJSON *user = root ? cJSON_GetObjectItem(root, "username") : NULL;
    cJSON *pass = root ? cJSON_GetObjectItem(root, "password") : NULL;
    cJSON *tlsVerify = root ? cJSON_GetObjectItem(root, "tlsVerify") : NULL;
    cJSON *caPem = root ? cJSON_GetObjectItem(root, "caPem") : NULL;
    if (!cJSON_IsString(host) || host->valuestring[0] == '\0' || strlen(host->valuestring) >= 64) {
        cJSON_Delete(root);
        return send_err(req, "invalid host");
    }
    if (cJSON_IsString(user) && strlen(user->valuestring) >= 65) {
        cJSON_Delete(root);
        return send_err(req, "username too long");
    }
    if (cJSON_IsString(pass) && strlen(pass->valuestring) >= 65) {
        cJSON_Delete(root);
        return send_err(req, "password too long");
    }
    if (cJSON_IsString(caPem) && strlen(caPem->valuestring) >= MQTT_CA_PEM_MAX) {
        cJSON_Delete(root);
        return send_err(req, "caPem too long");
    }
    (void)cid; /* clientId is device-bound; ignore client overrides */

    mqttAttr_t mqtt;
    cfg_get_mqtt_attr(&mqtt);
    strlcpy(mqtt.host, host->valuestring, sizeof(mqtt.host));
    if (cJSON_IsString(transport)) {
        mqtt.transport = mqtt_transport_from_str(transport->valuestring);
    } else if (cJSON_IsBool(tls)) {
        mqtt.transport = cJSON_IsTrue(tls) ? MQTT_TRANSPORT_TLS : MQTT_TRANSPORT_TCP;
    }
    if (cJSON_IsNumber(port)) {
        if (port->valueint <= 0 || port->valueint >= 65536) {
            cJSON_Delete(root);
            return send_err(req, "invalid port");
        }
        mqtt.port = port->valueint;
    } else {
        mqtt.port = mqtt_default_port(mqtt.transport);
    }
    {
        /* Keep SoftAP / NeoMind identity aligned: ek410-{deviceId}. */
        devInfo_t info;
        cfg_get_dev_info(&info);
        snprintf(mqtt.clientId, sizeof(mqtt.clientId), "ek410-%s", info.deviceId);
    }
    if (cJSON_IsNumber(qos) && qos->valueint >= 0 && qos->valueint <= 2) {
        mqtt.qos = qos->valueint;
    }
    if (cJSON_IsString(user)) {
        strlcpy(mqtt.username, user->valuestring, sizeof(mqtt.username));
    }
    /* Empty password in the form means "unchanged" — browsers often leave the
     * field blank; clearing NVS here broke auth and wedged Testing MQTT. */
    if (cJSON_IsString(pass) && pass->valuestring[0] != '\0') {
        strlcpy(mqtt.password, pass->valuestring, sizeof(mqtt.password));
    }
    if (cJSON_IsBool(tlsVerify)) {
        mqtt.tlsVerify = cJSON_IsTrue(tlsVerify);
    }

    mqttAttr_t prev;
    cfg_get_mqtt_attr(&prev);
    /* A blank password keeps the stored secret only for the same account:
     * never send it to a different broker or username. */
    const bool passwordCleared =
        !(cJSON_IsString(pass) && pass->valuestring[0] != '\0') &&
        mqtt.password[0] != '\0' &&
        (strcmp(prev.host, mqtt.host) != 0 || strcmp(prev.username, mqtt.username) != 0);
    if (passwordCleared) {
        mqtt.password[0] = '\0';
    }
    const bool same_broker =
        prev.transport == mqtt.transport &&
        prev.port == mqtt.port &&
        prev.qos == mqtt.qos &&
        prev.tlsVerify == mqtt.tlsVerify &&
        strcmp(prev.host, mqtt.host) == 0 &&
        strcmp(prev.username, mqtt.username) == 0 &&
        strcmp(prev.password, mqtt.password) == 0;
    const bool ca_touch = cJSON_IsString(caPem) && caPem->valuestring[0] != '\0';
    /* Test button always POSTs setMqttParam first. If nothing changed and we
     * are already online, skip stop/restart. */
    const bool keep_live = same_broker && !ca_touch && mqtt_is_connected() &&
                           wifi_sta_is_connected();

    const bool stopped_for_save = wifi_sta_is_connected() && !keep_live;
    if (stopped_for_save) {
        /* Stop old client BEFORE persist — otherwise a busy restart leaves NVS
         * on the new broker while the live client still talks to the old one. */
        if (mqtt_stop_wait(4000) != 0) {
            cJSON_Delete(root);
            return send_err(req, "mqtt stop busy");
        }
    }
    /* CA before broker attrs: if CA NVS fails, leave host/port unchanged. */
    if (cJSON_IsString(caPem)) {
        if (cfg_set_mqtt_ca_pem(caPem->valuestring) != ESP_OK) {
            cJSON_Delete(root);
            if (stopped_for_save) {
                mqtt_restore_after_temp_stop(); /* NVS host unchanged */
            }
            return send_err(req, "mqtt CA save failed");
        }
    }
    if (cfg_set_mqtt_attr(&mqtt) != ESP_OK) {
        cJSON_Delete(root);
        if (stopped_for_save) {
            mqtt_restore_after_temp_stop(); /* prior NVS broker */
        }
        return send_err(req, "mqtt config save failed");
    }
    cJSON_Delete(root);
    /* SoftAP+STA: mqtt_bringup may pause httpd — send HTTP response FIRST so
     * SoftAP clients do not lose the setMqttParam reply mid-pause. */
    const bool softap = wifi_softap_is_up();
    const bool will_start = wifi_sta_is_connected() && !keep_live;
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddNumberToObject(resp, "code", 0);
    if (softap && keep_live) {
        /* Already online — SoftAP UI must not fall through to blocking testMqtt. */
        cJSON_AddStringToObject(resp, "msg", "ok — MQTT session already up");
        cJSON_AddBoolToObject(resp, "mqttSoftApSession", true);
    } else if (softap && will_start) {
        cJSON_AddStringToObject(resp, "msg", "ok — connecting MQTT over SoftAP (brief web pause)");
        cJSON_AddBoolToObject(resp, "mqttSoftApConnect", true);
    } else if (softap && !wifi_sta_is_connected()) {
        cJSON_AddStringToObject(resp, "msg", "saved — join home Wi-Fi to connect MQTT");
        cJSON_AddBoolToObject(resp, "mqttDeferred", true);
    } else {
        cJSON_AddStringToObject(resp, "msg", "ok");
    }
    /* Fits the httpd task stack: both messages together stay under 90 chars. */
    char warn[96] = "";
    if (passwordCleared) {
        strlcpy(warn, "password cleared: broker or username changed", sizeof(warn));
    }
    if (mqtt.transport == MQTT_TRANSPORT_TCP && mqtt.port == 8883) {
        snprintf(warn + strlen(warn), sizeof(warn) - strlen(warn),
                 "%sport 8883 with mqtt (expected mqtts?)", warn[0] ? "; " : "");
    } else if (mqtt.transport == MQTT_TRANSPORT_TLS && mqtt.port == 1883) {
        snprintf(warn + strlen(warn), sizeof(warn) - strlen(warn),
                 "%sport 1883 with mqtts (expected mqtt?)", warn[0] ? "; " : "");
    }
    if (warn[0]) {
        cJSON_AddStringToObject(resp, "warn", warn);
    }
    char *str = cJSON_PrintUnformatted(resp);
    esp_err_t res = send_json(req, str);
    cJSON_free(str);
    cJSON_Delete(resp);
    if (will_start) {
        /* SoftAP web-first defer must not block an explicit save/test. */
        wifi_softap_mqtt_user_start();
        /* The last attempt's failure (e.g. "bad username or password") must
         * not be read back as the verdict on the config just saved. */
        mqtt_forget_error();
        /* Let the SoftAP TCP stack flush the response before pause. */
        vTaskDelay(pdMS_TO_TICKS(80));
        mqtt_start();
    }
    return res;
}

static void softap_close_deferred_task(void *arg)
{
    (void)arg;
    /* Let the HTTP response reach the phone before tearing down SoftAP. */
    vTaskDelay(pdMS_TO_TICKS(600));
    wifi_softap_close();
    vTaskDelete(NULL);
}

static esp_err_t close_softap_handle(httpd_req_t *req)
{
    (void)req;
    if (!wifi_softap_is_up()) {
        return send_json(req, "{\"code\":0,\"msg\":\"already off\",\"softapUp\":false}");
    }
    esp_err_t res = send_json(req, "{\"code\":0,\"msg\":\"SoftAP closing\",\"softapUp\":false}");
    if (xTaskCreate(softap_close_deferred_task, "ap_off", 4096, NULL, 5, NULL) != pdPASS) {
        wifi_softap_close();
    }
    return res;
}

static void softap_open_deferred_task(void *arg)
{
    (void)arg;
    wifi_softap_open();
    vTaskDelete(NULL);
}

static esp_err_t open_softap_handle(httpd_req_t *req)
{
    softap_touch();
    if (wifi_softap_is_up()) {
        wifi_softap_touch();
        return send_json(req, "{\"code\":0,\"msg\":\"already on\",\"softapUp\":true}");
    }
    esp_err_t res = send_json(req, "{\"code\":0,\"msg\":\"SoftAP opening\",\"softapUp\":true}");
    if (xTaskCreate(softap_open_deferred_task, "ap_on", 10240, NULL, 5, NULL) != pdPASS) {
        wifi_softap_open();
    }
    return res;
}

static void ble_open_deferred_task(void *arg)
{
    (void)arg;
    /* Let the openBleProv HTTP response finish and the TCP session close
     * before httpd_stop — otherwise BLE enter can stall forever. */
    vTaskDelay(pdMS_TO_TICKS(800));
    wifi_ble_prov_open();
    vTaskDelete(NULL);
}

static void ble_close_deferred_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(300));
    wifi_ble_prov_close();
    vTaskDelete(NULL);
}

static esp_err_t open_ble_handle(httpd_req_t *req)
{
    if (wifi_ble_prov_is_up()) {
        wifi_ble_prov_touch();
        return send_json(req, "{\"code\":0,\"msg\":\"already on\",\"bleProvUp\":true}");
    }
    /* Optional {"idleSec":N} for CI auto-exit. */
    char *body = read_body(req);
    if (body) {
        cJSON *root = json_guard_parse(body);
        free(body);
        if (root) {
            cJSON *idle = cJSON_GetObjectItem(root, "idleSec");
            if (cJSON_IsNumber(idle) && idle->valuedouble > 0) {
                cfg_ble_idle_sec_set((int)idle->valuedouble);
            }
            cJSON_Delete(root);
        }
    }
    esp_err_t res = send_json(req, "{\"code\":0,\"msg\":\"BLE starting\",\"bleProvUp\":true}");
    if (xTaskCreate(ble_open_deferred_task, "ble_on", 8192, NULL, 5, NULL) != pdPASS) {
        wifi_ble_prov_open();
    }
    return res;
}

static esp_err_t close_ble_handle(httpd_req_t *req)
{
    if (!wifi_ble_prov_is_up()) {
        return send_json(req, "{\"code\":0,\"msg\":\"already off\",\"bleProvUp\":false}");
    }
    esp_err_t res = send_json(req, "{\"code\":0,\"msg\":\"BLE closing\",\"bleProvUp\":false}");
    if (xTaskCreate(ble_close_deferred_task, "ble_off", 8192, NULL, 5, NULL) != pdPASS) {
        wifi_ble_prov_close();
    }
    return res;
}

/*
 * SoftAP + mqtt_test_ping must not run on an httpd worker: bringup pauses
 * httpd to free internal heap, but stop cannot finish while this handler
 * holds the worker → "httpd pause incomplete" deadlock. Respond first, then
 * probe from a deferred task after the TCP reply has flushed.
 */
static void softap_mqtt_test_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(120));
    char err[96];
    int rc = mqtt_test_ping(12000, err, sizeof(err));
    ESP_LOGI(TAG, "SoftAP deferred MQTT test rc=%d %s", rc, err[0] ? err : "");
    vTaskDelete(NULL);
}

static esp_err_t test_mqtt_handle(httpd_req_t *req)
{
    softap_touch();
    char err[96];
    err[0] = '\0';

    if (wifi_softap_is_up()) {
        if (!wifi_sta_is_connected()) {
            return send_json(req,
                "{\"code\":1,\"ok\":false,\"connected\":false,"
                "\"msg\":\"need home Wi-Fi (STA) — SoftAP alone cannot reach broker\"}");
        }
        if (mqtt_is_connected() && wifi_sta_is_connected()) {
            return send_json(req,
                "{\"code\":0,\"ok\":true,\"connected\":true,\"softapAsync\":false,"
                "\"msg\":\"ok — MQTT session already up\"}");
        }
        /* Kick bringup after the HTTP response leaves this worker. */
        esp_err_t res = send_json(req,
            "{\"code\":0,\"ok\":false,\"connected\":false,\"connecting\":true,"
            "\"softapAsync\":true,"
            "\"msg\":\"SoftAP MQTT starting — poll getDevInfo (httpd must release first)\"}");
        if (xTaskCreate(softap_mqtt_test_task, "ap_mqtt_t", 4096, NULL, 5, NULL) != pdPASS) {
            wifi_softap_mqtt_user_start();
            mqtt_start();
            ESP_LOGW(TAG, "SoftAP MQTT test task create failed — mqtt_start only");
        }
        return res;
    }

    /* LAN: short sync probe is safe (no SoftAP httpd pause). */
    int rc = mqtt_test_ping(5000, err, sizeof(err));
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(obj, "code", rc == 0 ? 0 : 1);
    cJSON_AddStringToObject(obj, "msg", err[0] ? err : (rc == 0 ? "ok" : "failed"));
    cJSON_AddBoolToObject(obj, "ok", rc == 0);
    cJSON_AddBoolToObject(obj, "connected",
                          mqtt_is_connected() && wifi_sta_is_connected());
    char *str = cJSON_PrintUnformatted(obj);
    esp_err_t res = send_json(req, str);
    cJSON_free(str);
    cJSON_Delete(obj);
    return res;
}

/* ---------- display ---------- */

static esp_err_t display_render_text_handle(httpd_req_t *req)
{
    char *body = read_body(req);
    if (!body) {
        return send_err(req, "bad body");
    }
    cJSON *root = json_guard_parse(body);
    free(body);
    cJSON *t = root ? cJSON_GetObjectItem(root, "text") : NULL;
    cJSON *c = root ? cJSON_GetObjectItem(root, "color") : NULL;
    cJSON *bg = root ? cJSON_GetObjectItem(root, "bg") : NULL;
    cJSON *sc = root ? cJSON_GetObjectItem(root, "scale") : NULL;
    if (!cJSON_IsString(t)) {
        cJSON_Delete(root);
        return send_err(req, "missing text");
    }
    espd_render_t *render = calloc(1, sizeof(*render));
    if (!render) {
        cJSON_Delete(root);
        return send_err(req, "oom");
    }
    size_t tlen = strlen(t->valuestring);
    canvas_utf8_strlcpy(render->text, t->valuestring, sizeof(render->text));
    render->fg = cJSON_IsString(c) ? canvas_color(c->valuestring) : 0x00;
    render->bg = cJSON_IsString(bg) ? canvas_color(bg->valuestring) : 0x11;
    render->scale = cJSON_IsNumber(sc) ? sc->valueint : 3;
    ESP_LOGI(TAG, "render_text queue %u bytes scale=%d",
             (unsigned)tlen, render->scale);
    int rc = app_epd_render_async(render);
    free(render);
    cJSON_Delete(root);
    if (rc != 0) {
        return send_err(req, app_epd_queue_err(rc));
    }
    return send_json(req, "{\"code\":0,\"msg\":\"queued\"}");
}

static esp_err_t display_test_pattern_handle(httpd_req_t *req)
{
    int rc = app_epd_test_pattern_async();
    if (rc != 0) {
        return send_err(req, app_epd_queue_err(rc));
    }
    return send_json(req, "{\"code\":0,\"msg\":\"queued\"}");
}

static esp_err_t display_render_canvas_handle(httpd_req_t *req)
{
    /* body is the canvas JSON itself ({"bg":..,"items":[..]}); allow it
     * wrapped in {"canvas":{...}} too. canvas_t is large — heap only. */
    char *body = read_body(req);
    if (!body) {
        return send_err(req, "bad body");
    }
    cJSON *root = json_guard_parse(body);
    free(body);
    if (!root) {
        return send_err(req, "invalid JSON");
    }
    cJSON *canvasRoot = root;
    cJSON *wrap = cJSON_GetObjectItem(root, "canvas");
    if (wrap) {
        canvasRoot = wrap;
    }
    canvas_t *cv = calloc(1, sizeof(*cv));
    if (!cv) {
        cJSON_Delete(root);
        return send_err(req, "oom");
    }
    char err[64];
    if (canvas_parse_json(canvasRoot, cv, err, sizeof(err)) != 0) {
        free(cv);
        cJSON_Delete(root);
        return send_err(req, err);
    }
    cJSON_Delete(root);
    int rc = app_epd_canvas_async(cv);
    free(cv);
    if (rc != 0) {
        return send_err(req, app_epd_queue_err(rc));
    }
    return send_json(req, "{\"code\":0,\"msg\":\"queued\"}");
}

static esp_err_t display_url_handle(httpd_req_t *req)
{
    char *body = read_body(req);
    if (!body) {
        return send_err(req, "bad body");
    }
    cJSON *root = json_guard_parse(body);
    free(body);
    cJSON *u = root ? cJSON_GetObjectItem(root, "url") : NULL;
    if (!cJSON_IsString(u) || !u->valuestring[0]) {
        cJSON_Delete(root);
        return send_err(req, "missing url");
    }
    if (strlen(u->valuestring) >= 512) {
        cJSON_Delete(root);
        return send_err(req, "url too long");
    }
    int rc = app_epd_url_async(u->valuestring);
    cJSON_Delete(root);
    if (rc != 0) {
        return send_err(req, app_epd_queue_err(rc));
    }
    /* Accepted onto the render queue — panel refresh finishes later. */
    return send_json(req, "{\"code\":0,\"msg\":\"queued\"}");
}

/* SoftAP SPA preview: STA fetch via boot-time img_proxy task (TLS stack). */
static volatile bool s_proxy_busy;

static esp_err_t display_proxy_image_handle(httpd_req_t *req)
{
    softap_touch();
    if (!wifi_sta_is_connected()) {
        return send_err(req, "need STA Wi-Fi");
    }
    if (s_proxy_busy) {
        ESP_LOGW(TAG, "proxyImage rejected: busy");
        return send_err(req, "proxy busy");
    }

    char *body = read_body(req);
    if (!body) {
        return send_err(req, "bad body");
    }
    cJSON *root = json_guard_parse(body);
    free(body);
    cJSON *u = root ? cJSON_GetObjectItem(root, "url") : NULL;
    if (!cJSON_IsString(u) || !u->valuestring[0]) {
        cJSON_Delete(root);
        return send_err(req, "missing url");
    }
    if (strlen(u->valuestring) >= 512) {
        cJSON_Delete(root);
        return send_err(req, "url too long");
    }
    if (strncmp(u->valuestring, "http://", 7) != 0 &&
        strncmp(u->valuestring, "https://", 8) != 0) {
        cJSON_Delete(root);
        return send_err(req, "url must be http(s)");
    }

    epd_proxy_job_t *job = calloc(1, sizeof(*job));
    if (!job) {
        cJSON_Delete(root);
        return send_err(req, "OOM");
    }
    strlcpy(job->url, u->valuestring, sizeof(job->url));
    cJSON_Delete(root);

    s_proxy_busy = true;
    ESP_LOGI(TAG, "proxyImage queue %.96s%s", job->url, strlen(job->url) > 96 ? "…" : "");
    /* May wait behind splash/panel refresh (20–40s) then TLS download. On
     * failure job may already be freed: use the out-params, not *job. */
    char perr[48] = "";
    bool proxy_released = false;
    int rc = app_epd_proxy_fetch(job, 90000, perr, sizeof(perr), &proxy_released);
    if (rc != 0) {
        char errcopy[48];
        strlcpy(errcopy, perr[0] ? perr : "proxy fail", sizeof(errcopy));
        ESP_LOGW(TAG, "proxyImage fail: %s", errcopy);
        if (!proxy_released) {
            free(job->data);
            free(job);
        }
        s_proxy_busy = false;
        return send_err(req, errcopy);
    }

    softap_touch();
    httpd_resp_set_type(req, job->ctype[0] ? job->ctype : "application/octet-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t res = httpd_resp_send(req, (const char *)job->data, job->len);
    free(job->data);
    free(job);
    s_proxy_busy = false;
    return res;
}

static esp_err_t display_upload_frame_handle(httpd_req_t *req)
{
    if (req->content_len != 960000) {
        return send_err(req, "body must be a raw 960000-byte frame");
    }
    if (ota_busy()) {
        return send_err(req, "ota busy");
    }
    if (!app_epd_jobs_accepted()) {
        return send_err(req, "sleep pending");
    }
    if (epd_upload_start() != 0) {
        return send_err(req, "panel busy");
    }
    unsigned char *buf = app_epd_dma_buf();
    int bufSize = app_epd_dma_buf_size();
    int remaining = req->content_len;
    while (remaining > 0) {
        int want = remaining > bufSize ? bufSize : remaining;
        int got = httpd_req_recv(req, (char *)buf, want);
        if (got <= 0) {
            epd_upload_abort();
            return send_err(req, "upload interrupted");
        }
        if (epd_upload_write(buf, got) != 0) {
            epd_upload_abort();
            return send_err(req, "panel write failed");
        }
        remaining -= got;
    }
    if (epd_upload_finish() != 0) { /* sync refresh + unlock */
        return send_err(req, "upload finish failed");
    }
    return send_json(req, "{\"code\":0,\"msg\":\"uploaded, refreshing\"}");
}

static esp_err_t display_last_frame_meta_handle(httpd_req_t *req)
{
    softap_touch();
    last_frame_meta_t m;
    cJSON *obj = cJSON_CreateObject();
    if (last_frame_get_meta(&m) != 0 || !m.present) {
        cJSON_AddBoolToObject(obj, "present", false);
        cJSON_AddNumberToObject(obj, "bytes", LAST_FRAME_BYTES);
    } else {
        cJSON_AddBoolToObject(obj, "present", true);
        cJSON_AddNumberToObject(obj, "seq", (double)m.seq);
        cJSON_AddNumberToObject(obj, "crc32", (double)m.crc32);
        cJSON_AddNumberToObject(obj, "savedUs", (double)m.saved_us);
        cJSON_AddNumberToObject(obj, "bytes", LAST_FRAME_BYTES);
        cJSON_AddStringToObject(obj, "source", m.source);
    }
    char *s = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (!s) {
        return send_err(req, "OOM");
    }
    esp_err_t res = send_json(req, s);
    free(s);
    return res;
}

typedef struct {
    httpd_req_t *req;
    const last_frame_meta_t *meta;
    bool hdr_sent;
} last_frame_http_ctx_t;

static int last_frame_http_writer(const uint8_t *data, size_t len, void *ctx)
{
    last_frame_http_ctx_t *c = (last_frame_http_ctx_t *)ctx;
    if (!c->hdr_sent) {
        httpd_resp_set_type(c->req, "application/octet-stream");
        httpd_resp_set_hdr(c->req, "Cache-Control", "no-store");
        httpd_resp_set_hdr(c->req, "X-EK410-Frame", "native-4bpp-960000");
        char src_hdr[40];
        snprintf(src_hdr, sizeof(src_hdr), "%s",
                 (c->meta && c->meta->source[0]) ? c->meta->source : "panel");
        httpd_resp_set_hdr(c->req, "X-EK410-Source", src_hdr);
        c->hdr_sent = true;
    }
    if (httpd_resp_send_chunk(c->req, (const char *)data, len) != ESP_OK) {
        return -1;
    }
    return 0;
}

static esp_err_t display_last_frame_handle(httpd_req_t *req)
{
    softap_touch();
    last_frame_meta_t m;
    if (last_frame_get_meta(&m) != 0 || !m.present) {
        return send_err(req, "no last frame");
    }

    /* Stream from flash in 4KB chunks — a one-shot 960KB alloc+send stalls SoftAP
     * and causes browser "Failed to fetch" under concurrent page loads. */
    last_frame_http_ctx_t ctx = { .req = req, .meta = &m, .hdr_sent = false };
    int rc = last_frame_stream(last_frame_http_writer, &ctx);
    if (rc == -2) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return send_err(req, "last frame busy");
    }
    if (rc != 0) {
        if (ctx.hdr_sent) {
            httpd_resp_send_chunk(req, NULL, 0);
            return ESP_FAIL;
        }
        return send_err(req, "last frame corrupt");
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

/* ---------- OTA ---------- */

static esp_err_t set_dev_upgrade_handle(httpd_req_t *req)
{
    if (ota_busy()) {
        return send_err(req, "ota busy");
    }
    char *body = read_body(req);
    if (!body) {
        return send_err(req, "bad body");
    }
    cJSON *root = json_guard_parse(body);
    free(body);
    cJSON *url = root ? cJSON_GetObjectItem(root, "url") : NULL;
    if (!cJSON_IsString(url) || strncmp(url->valuestring, "https://", 8) != 0) {
        cJSON_Delete(root);
        return send_err(req, "url must be https://");
    }
    char *urlCopy = strdup(url->valuestring);
    cJSON_Delete(root);
    if (!urlCopy) {
        return send_err(req, "oom");
    }
    if (strlen(urlCopy) >= 512) {
        free(urlCopy);
        return send_err(req, "url too long");
    }
    /* Claim BEFORE stopping MQTT: that stop arms a start-retry, which must
     * already see the OTA and keep the client down until the download ends. */
    if (!ota_url_claim()) {
        free(urlCopy);
        return send_err(req, "ota busy");
    }
    /* Match local upload: free internal heap before HTTPS TLS + flash write. */
    if (mqtt_stop_wait(4000) != 0) {
        free(urlCopy);
        ota_url_release(NULL);
        return send_err(req, "mqtt stop busy");
    }
    sleep_cancel(); /* don't sleep mid-upgrade */
    /* The download runs on the render task: it owns the one deep TLS stack,
     * and a dedicated 12 KB task could not be created on a unit with Wi-Fi,
     * httpd and MQTT up (the request failed with "task spawn failed"). */
    int rc = app_epd_ota_async(urlCopy);
    free(urlCopy);
    if (rc != 0) {
        ota_url_release(NULL);
        ota_restore_runtime();
        return send_err(req, app_epd_queue_err(rc));
    }
    return send_json(req, "{\"code\":0,\"msg\":\"upgrading, device will reboot\"}");
}

/* Raw firmware body (application/octet-stream). Prefer LAN/STA; SoftAP works
 * but is slow. Success → JSON then reboot into pending-verify slot. */
static esp_err_t upload_firmware_handle(httpd_req_t *req)
{
    softap_touch();
    if (ota_busy()) {
        return send_err(req, "ota busy");
    }
    if (req->content_len <= 0) {
        return send_err(req, "Content-Length required");
    }
    /* Slot is 4MB; reject absurdly small stubs early (real app ≫ 200KB). */
    if (req->content_len < 200 * 1024) {
        return send_err(req, "image too small");
    }
    if (req->content_len > 4 * 1024 * 1024) {
        return send_err(req, "image too large");
    }

    /* Refuse to start if MQTT cannot stop — keep live session + sleep arm. */
    if (mqtt_stop_wait(4000) != 0) {
        return send_err(req, "mqtt stop busy");
    }
    sleep_cancel();

    char err[64] = "";
    if (ota_upload_begin((size_t)req->content_len, err, sizeof(err)) != 0) {
        ota_restore_runtime();
        return send_err(req, err[0] ? err : "ota begin failed");
    }

    led_set(LEDC_OTA_R, LED_BLINK_FAST);
    ESP_LOGI(TAG, "firmware upload %d bytes (softap=%d)",
             req->content_len, wifi_softap_is_up() ? 1 : 0);

    uint8_t *buf = malloc(4096);
    if (!buf) {
        ota_upload_abort();
        led_pulse(LEDC_ERR_R, 2000);
        ota_restore_runtime();
        return send_err(req, "oom");
    }

    int remaining = req->content_len;
    while (remaining > 0) {
        int want = remaining > 4096 ? 4096 : remaining;
        int got = httpd_req_recv(req, (char *)buf, want);
        if (got <= 0) {
            free(buf);
            ota_upload_abort();
            led_pulse(LEDC_ERR_R, 2000);
            ota_restore_runtime();
            return send_err(req, "upload interrupted");
        }
        if (ota_upload_write(buf, (size_t)got) != 0) {
            free(buf);
            ota_upload_abort();
            led_pulse(LEDC_ERR_R, 2000);
            ota_restore_runtime();
            return send_err(req, "ota write failed (bad magic or flash)");
        }
        remaining -= got;
        softap_touch();
    }
    free(buf);

    if (ota_upload_finish(err, sizeof(err)) != 0) {
        led_pulse(LEDC_ERR_R, 2000);
        ota_restore_runtime();
        return send_err(req, err[0] ? err : "ota validate failed");
    }

    esp_err_t res = send_json(req,
        "{\"code\":0,\"msg\":\"uploaded — rebooting into new image (pending verify)\"}");
    vTaskDelay(pdMS_TO_TICKS(400));
    esp_restart();
    return res; /* not reached */
}

/* ---------- power ---------- */

static esp_err_t get_power_param_handle(httpd_req_t *req)
{
    powerAttr_t p;
    cfg_get_power_attr(&p);
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "mode", power_mode_str(p.mode));
    cJSON_AddNumberToObject(obj, "sleepMin", p.sleepMin);
    cJSON_AddNumberToObject(obj, "wakeSec", p.wakeSec);
    cJSON_AddStringToObject(obj, "wakeUrl", p.wakeUrl);
    cJSON_AddStringToObject(obj, "imageQuality", img_quality_str(p.imageQuality));
    cJSON_AddNumberToObject(obj, "batteryAdcGpio", p.batteryAdcGpio);
    cJSON_AddNumberToObject(obj, "batteryDivMv", p.batteryDivMv);
    cJSON_AddNumberToObject(obj, "ledGpio", p.ledGpio);
    cJSON_AddBoolToObject(obj, "sleepArmed", sleep_is_armed());
    char *str = cJSON_PrintUnformatted(obj);
    esp_err_t res = send_json(req, str);
    cJSON_free(str);
    cJSON_Delete(obj);
    return res;
}

static esp_err_t set_power_param_handle(httpd_req_t *req)
{
    char *body = read_body(req);
    if (!body) {
        return send_err(req, "bad body");
    }
    cJSON *root = json_guard_parse(body);
    free(body);
    if (!root) {
        return send_err(req, "invalid JSON");
    }
    powerAttr_t p;
    cfg_get_power_attr(&p);
    cJSON *v;
    if ((v = cJSON_GetObjectItem(root, "mode")) && cJSON_IsString(v)) {
        p.mode = power_mode_from_str(v->valuestring);
    } else if ((v = cJSON_GetObjectItem(root, "sleepMin")) && cJSON_IsNumber(v)) {
        /* Legacy: sleepMin>0 implies cycle, 0 implies always. */
        p.sleepMin = v->valueint < 0 ? 0 : v->valueint;
        p.mode = p.sleepMin > 0 ? POWER_MODE_CYCLE : POWER_MODE_ALWAYS;
    }
    if ((v = cJSON_GetObjectItem(root, "sleepMin")) && cJSON_IsNumber(v)) {
        p.sleepMin = v->valueint < 0 ? 0 : v->valueint;
    }
    if ((v = cJSON_GetObjectItem(root, "wakeSec")) && cJSON_IsNumber(v)) {
        p.wakeSec = v->valueint;
    }
    if ((v = cJSON_GetObjectItem(root, "wakeUrl")) && cJSON_IsString(v)) {
        strlcpy(p.wakeUrl, v->valuestring, sizeof(p.wakeUrl));
    }
    if ((v = cJSON_GetObjectItem(root, "imageQuality"))) {
        if (cJSON_IsString(v)) {
            p.imageQuality = img_quality_from_str(v->valuestring);
        } else if (cJSON_IsNumber(v)) {
            p.imageQuality = (v->valueint == 0) ? IMG_QUALITY_SPEED : IMG_QUALITY_QUALITY;
        }
    }
    if ((v = cJSON_GetObjectItem(root, "batteryAdcGpio")) && cJSON_IsNumber(v)) {
        p.batteryAdcGpio = v->valueint;
    }
    if ((v = cJSON_GetObjectItem(root, "batteryDivMv")) && cJSON_IsNumber(v) && v->valueint > 0) {
        p.batteryDivMv = v->valueint;
    }
    if ((v = cJSON_GetObjectItem(root, "ledGpio")) && cJSON_IsNumber(v) && v->valueint >= 0) {
        p.ledGpio = v->valueint;
    }
    /* A body that names no mode only tunes fields (quality, wakeUrl, wakeSec):
     * the stored sleep policy must not be re-applied as a fresh request. */
    const bool policyGiven =
        cJSON_IsString(cJSON_GetObjectItem(root, "mode")) ||
        cJSON_IsNumber(cJSON_GetObjectItem(root, "sleepMin"));
    cJSON_Delete(root);
    esp_err_t perr = cfg_set_power_attr(&p);
    cfg_get_power_attr(&p); /* re-read normalized values */
    if (perr != ESP_OK) {
        return send_err(req, "power save failed");
    }
    evtlog_event("SYS", "power mode=%s sleepMin=%d wakeSec=%d img=%s",
                 power_mode_str(p.mode), p.sleepMin, p.wakeSec,
                 img_quality_str(p.imageQuality));

    bool sleepSoon = false;
    const char *sleepDeferred = NULL;
    if (!policyGiven) {
        /* Field-only edit: keep the current schedule as it is — at most
         * refresh an already armed window (wakeSec may have changed). */
        if (p.mode == POWER_MODE_CYCLE && sleep_is_armed()) {
            sleep_arm_from_config();
        }
    } else if (p.mode == POWER_MODE_CYCLE) {
        sleep_set_hibernate_pending(false);
        if (wifi_softap_is_up()) {
            ESP_LOGW(TAG, "SoftAP up — sleep schedule deferred until AP closes");
            sleep_cancel();
            sleepDeferred = "softap";
        } else if (wifi_sta_is_connected()) {
            sleep_arm_from_config();
        } else {
            ESP_LOGW(TAG, "not online — sleep schedule deferred until GOT_IP");
            sleep_cancel();
            sleepDeferred = "no_sta";
            /* Still bounded: sleep after the max window if STA never joins. */
            sleep_arm_offline_fallback();
        }
    } else if (p.mode == POWER_MODE_HIBERNATE) {
        sleep_cancel();
        if (wifi_softap_is_up()) {
            sleepDeferred = "softap";
            /* sleep_on_prov_closed() enters it when the hotspot closes. */
            sleep_set_hibernate_pending(true);
        } else {
            sleepSoon = true;
        }
    } else {
        sleep_set_hibernate_pending(false);
        sleep_cancel();
    }
    if (sleepSoon) {
        /* Spawn first so spawn failure is not reported as success; sleep_go
         * still waits on the panel before tearing down Wi‑Fi / MQTT. */
        if (!sleep_now()) {
            return send_err(req, "sleep failed");
        }
        return send_json(req, "{\"code\":0,\"msg\":\"hibernating\"}");
    }
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddNumberToObject(resp, "code", 0);
    cJSON_AddStringToObject(resp, "msg", sleepDeferred ? "saved — sleep deferred" : "ok");
    cJSON_AddBoolToObject(resp, "sleepArmed", sleep_is_armed());
    if (sleepDeferred) {
        cJSON_AddStringToObject(resp, "sleepDeferred", sleepDeferred);
    }
    char *str = cJSON_PrintUnformatted(resp);
    esp_err_t res = send_json(req, str);
    cJSON_free(str);
    cJSON_Delete(resp);
    return res;
}

static esp_err_t sleep_now_handle(httpd_req_t *req)
{
    softap_touch();
    if (wifi_softap_is_up()) {
        return send_err(req, "softap busy — close hotspot first");
    }
    /* Spawn first so spawn failure is not reported as success; sleep_go
     * still waits on the panel before tearing down Wi‑Fi / MQTT. */
    if (!sleep_now()) {
        return send_err(req, "sleep failed");
    }
    return send_json(req, "{\"code\":0,\"msg\":\"sleeping\"}");
}

static esp_err_t get_event_log_meta_handle(httpd_req_t *req)
{
    softap_touch();
    evtlog_meta_t m;
    evtlog_get_meta(&m);
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddBoolToObject(obj, "present", m.present);
    cJSON_AddNumberToObject(obj, "bytes", m.bytes);
    cJSON_AddNumberToObject(obj, "seq", m.seq);
    cJSON_AddNumberToObject(obj, "capBytes", m.cap_bytes);
    cJSON_AddNumberToObject(obj, "keepDays", m.keep_days);
    cJSON_AddBoolToObject(obj, "timeOk", m.time_ok);
    char *s = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (!s) {
        return send_err(req, "OOM");
    }
    esp_err_t res = send_json(req, s);
    free(s);
    return res;
}

typedef struct {
    httpd_req_t *req;
    bool hdr_sent;
} evtlog_http_ctx_t;

static int evtlog_http_writer(const uint8_t *data, size_t len, void *ctx)
{
    evtlog_http_ctx_t *c = (evtlog_http_ctx_t *)ctx;
    if (!c->hdr_sent) {
        httpd_resp_set_type(c->req, "text/plain; charset=utf-8");
        httpd_resp_set_hdr(c->req, "Content-Disposition",
                           "attachment; filename=\"ek410-event.log\"");
        httpd_resp_set_hdr(c->req, "Cache-Control", "no-store");
        c->hdr_sent = true;
    }
    if (len == 0) {
        return 0;
    }
    return httpd_resp_send_chunk(c->req, (const char *)data, len) == ESP_OK ? 0 : -1;
}

static esp_err_t get_event_log_handle(httpd_req_t *req)
{
    softap_touch();
    evtlog_meta_t m;
    evtlog_get_meta(&m);
    if (!m.present) {
        return send_err(req, "event log unavailable");
    }
    evtlog_http_ctx_t ctx = { .req = req, .hdr_sent = false };
    int rc = evtlog_stream(evtlog_http_writer, &ctx);
    if (rc == -2) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return send_err(req, "event log busy");
    }
    if (!ctx.hdr_sent) {
        /* Empty log still returns a downloadable file. */
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        httpd_resp_set_hdr(req, "Content-Disposition",
                           "attachment; filename=\"ek410-event.log\"");
        return httpd_resp_sendstr(req, "# EK410 event log (empty)\n");
    }
    if (rc != 0) {
        httpd_resp_send_chunk(req, NULL, 0);
        return ESP_FAIL;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t clear_event_log_handle(httpd_req_t *req)
{
    softap_touch();
    evtlog_meta_t m;
    evtlog_get_meta(&m);
    if (!m.present) {
        return send_err(req, "event log unavailable");
    }
    if (evtlog_clear() != 0) {
        return send_err(req, "event log clear failed");
    }
    return send_json(req, "{\"code\":0,\"msg\":\"ok\"}");
}

/* ---------- static page ---------- */

static esp_err_t get_root_handler(httpd_req_t *req)
{
    softap_touch();
    httpd_resp_set_type(req, "text/html");
    /*
     * INDEX is ~212KB raw. Pushing that over STA while MQTT holds Wi-Fi buffers
     * causes wifi:mem fail → accept(113) → otadata NO_MEM → stack panic/reboot.
     * Serve pre-gzipped blob (~58KB) with Content-Encoding so wire traffic stays
     * small; still chunk + yield so TX rings can drain.
     */
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    const size_t total = INDEX_HTML_GZ_LEN;
    const size_t chunk = 2048;
    for (size_t off = 0; off < total; off += chunk) {
        size_t n = total - off;
        if (n > chunk) {
            n = chunk;
        }
        if (httpd_resp_send_chunk(req, (const char *)(INDEX_HTML_GZ + off), n) != ESP_OK) {
            httpd_resp_send_chunk(req, NULL, 0);
            return ESP_FAIL;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

/* ---------- registration ---------- */

static const httpd_uri_t g_handlers[] = {
    { .uri = "/", .method = HTTP_GET, .handler = get_root_handler },
    { .uri = "/api/v1/system/getDevInfo", .method = HTTP_GET, .handler = get_dev_info_handle },
    { .uri = "/api/v1/system/getDiag", .method = HTTP_GET, .handler = get_diag_handle },
    { .uri = "/api/v1/system/setDevInfo", .method = HTTP_POST, .handler = set_dev_info_handle },
    { .uri = "/api/v1/system/setDevReset", .method = HTTP_POST, .handler = set_dev_reset_handle },
    { .uri = "/api/v1/system/setDevFactory", .method = HTTP_POST, .handler = set_dev_factory_handle },
    { .uri = "/api/v1/network/getWifiParam", .method = HTTP_GET, .handler = get_wifi_param_handle },
    { .uri = "/api/v1/network/setWifiParam", .method = HTTP_POST, .handler = set_wifi_param_handle },
    { .uri = "/api/v1/network/scanWifi", .method = HTTP_GET, .handler = scan_wifi_handle },
    { .uri = "/api/v1/network/getMqttParam", .method = HTTP_GET, .handler = get_mqtt_param_handle },
    { .uri = "/api/v1/network/setMqttParam", .method = HTTP_POST, .handler = set_mqtt_param_handle },
    { .uri = "/api/v1/network/testMqtt", .method = HTTP_POST, .handler = test_mqtt_handle },
    { .uri = "/api/v1/network/closeSoftAp", .method = HTTP_POST, .handler = close_softap_handle },
    { .uri = "/api/v1/network/openSoftAp", .method = HTTP_POST, .handler = open_softap_handle },
    { .uri = "/api/v1/network/openBleProv", .method = HTTP_POST, .handler = open_ble_handle },
    { .uri = "/api/v1/network/closeBleProv", .method = HTTP_POST, .handler = close_ble_handle },
    { .uri = "/api/v1/display/renderText", .method = HTTP_POST, .handler = display_render_text_handle },
    { .uri = "/api/v1/display/renderCanvas", .method = HTTP_POST, .handler = display_render_canvas_handle },
    { .uri = "/api/v1/display/uploadFrame", .method = HTTP_POST, .handler = display_upload_frame_handle },
    { .uri = "/api/v1/display/displayUrl", .method = HTTP_POST, .handler = display_url_handle },
    { .uri = "/api/v1/display/proxyImage", .method = HTTP_POST, .handler = display_proxy_image_handle },
    { .uri = "/api/v1/display/getLastFrameMeta", .method = HTTP_GET, .handler = display_last_frame_meta_handle },
    { .uri = "/api/v1/display/lastFrame", .method = HTTP_GET, .handler = display_last_frame_handle },
    { .uri = "/api/v1/display/testPattern", .method = HTTP_POST, .handler = display_test_pattern_handle },
    { .uri = "/api/v1/system/setDevUpgrade", .method = HTTP_POST, .handler = set_dev_upgrade_handle },
    { .uri = "/api/v1/system/uploadFirmware", .method = HTTP_POST, .handler = upload_firmware_handle },
    { .uri = "/api/v1/system/getPowerParam", .method = HTTP_GET, .handler = get_power_param_handle },
    { .uri = "/api/v1/system/setPowerParam", .method = HTTP_POST, .handler = set_power_param_handle },
    { .uri = "/api/v1/system/sleepNow", .method = HTTP_POST, .handler = sleep_now_handle },
    { .uri = "/api/v1/system/getEventLogMeta", .method = HTTP_GET, .handler = get_event_log_meta_handle },
    { .uri = "/api/v1/system/eventLog", .method = HTTP_GET, .handler = get_event_log_handle },
    { .uri = "/api/v1/system/clearEventLog", .method = HTTP_POST, .handler = clear_event_log_handle },
    /* NeoLink CT-Prov/1 SoftAP binding (§16) — also reachable on STA IP for tests. */
    { .uri = "/api/v1/ctprov", .method = HTTP_POST, .handler = ct_prov_http_handle },
};

static httpd_handle_t s_httpd;
static volatile bool s_httpdStopping;
static volatile bool s_httpdStopOk;
static SemaphoreHandle_t s_httpdStopDone;

bool http_server_is_up(void)
{
    return s_httpd != NULL;
}

bool http_server_is_stopping(void)
{
    return s_httpdStopping;
}

static void httpd_stop_done_ensure(void)
{
    if (!s_httpdStopDone) {
        s_httpdStopDone = xSemaphoreCreateBinary();
    }
}

static void httpd_stop_task(void *arg)
{
    (void)arg;
    /*
     * Keep s_httpd until httpd_stop() commits. IDF returns ESP_FAIL if the
     * shutdown ctrl msg fails — the server is still running; restore nothing,
     * just leave the handle so callers can retry.
     */
    httpd_handle_t h = s_httpd;
    if (!h) {
        s_httpdStopOk = true;
        s_httpdStopping = false;
        if (s_httpdStopDone) {
            xSemaphoreGive(s_httpdStopDone);
        }
        vTaskDelete(NULL);
        return;
    }
    esp_err_t err = httpd_stop(h);
    if (err == ESP_OK) {
        s_httpd = NULL;
        s_httpdStopOk = true;
        ESP_LOGI(TAG, "web server stopped");
    } else {
        s_httpdStopOk = false;
        ESP_LOGW(TAG, "httpd_stop: %s — handle kept", esp_err_to_name(err));
    }
    s_httpdStopping = false;
    if (s_httpdStopDone) {
        xSemaphoreGive(s_httpdStopDone);
    }
    vTaskDelete(NULL);
}

void http_server_stop(void)
{
    if (s_httpdStopping) {
        return;
    }
    if (!s_httpd) {
        return;
    }
    httpd_stop_done_ensure();
    if (s_httpdStopDone) {
        xSemaphoreTake(s_httpdStopDone, 0); /* clear stale */
    }
    s_httpdStopping = true;
    s_httpdStopOk = false;
    /*
     * httpd_stop() can block forever on lingering TCP sessions — never call it
     * synchronously. Do not clear s_httpd here; only httpd_stop_task may, and
     * only after a successful SDK stop.
     */
    for (int i = 0; i < 8; i++) {
        if (xTaskCreate(httpd_stop_task, "httpd_off", 2048, NULL, 6, NULL) == pdPASS) {
            ESP_LOGI(TAG, "web server stop armed");
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    ESP_LOGW(TAG, "httpd_off task create failed — handle kept");
    s_httpdStopping = false;
    s_httpdStopOk = false;
    if (s_httpdStopDone) {
        xSemaphoreGive(s_httpdStopDone);
    }
}

int http_server_stop_wait(int budget_ms)
{
    /* Cleared handle + not stopping ⇒ a prior stop succeeded (or never started). */
    if (!s_httpd && !s_httpdStopping) {
        return 0;
    }
    if (s_httpd && !s_httpdStopping) {
        http_server_stop();
    }
    /* Create failed or SDK stop failed → handle still owned, not stopping. */
    if (s_httpd && !s_httpdStopping) {
        return -1;
    }
    if (!s_httpdStopping) {
        /* Stop finished before wait — only success clears the handle. */
        return (!s_httpd && s_httpdStopOk) ? 0 : -1;
    }
    httpd_stop_done_ensure();
    if (!s_httpdStopDone) {
        vTaskDelay(pdMS_TO_TICKS(budget_ms > 0 ? budget_ms : 200));
        if (s_httpdStopping) {
            return -1;
        }
        return (!s_httpd && s_httpdStopOk) ? 0 : -1;
    }
    TickType_t ticks = pdMS_TO_TICKS(budget_ms > 0 ? budget_ms : 1);
    if (xSemaphoreTake(s_httpdStopDone, ticks) == pdTRUE) {
        return (!s_httpd && s_httpdStopOk) ? 0 : -1;
    }
    return -1;
}

void http_server_start(void)
{
    if (s_httpd) {
        return;
    }
    /* Do not race a still-running httpd_stop (port still bound). */
    if (s_httpdStopping) {
        (void)http_server_stop_wait(1500);
        if (s_httpdStopping) {
            ESP_LOGW(TAG, "httpd_start deferred — previous stop still running");
            return;
        }
    }
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = sizeof(g_handlers) / sizeof(g_handlers[0]);
    /* Canvas/upload + ctprov Wi-Fi scan need headroom beyond 4KB default.
     * Do NOT raise to 32KB — each httpd worker would exhaust C61 internal RAM.
     * 5K + static prov 4K .bss leaves room for httpd_start after Wi-Fi. */
    /* SoftAP + MQTT: leaner httpd. esp_http_server uses ONE server task for
     * multiple sockets — max_open_sockets cuts socket/lwIP cost, not N stacks. */
    const bool softap = wifi_softap_is_up();
    config.stack_size = softap ? 4608 : 5120;
    config.max_open_sockets = softap ? 2 : 3;
    /* setWifiParam may block up to ~20s while trying STA association */
    config.recv_wait_timeout = 30;
    config.send_wait_timeout = 30;
    config.lru_purge_enable = true;
    config.keep_alive_enable = false;
    esp_err_t err = httpd_start(&s_httpd, &config);
    if (err != ESP_OK || !s_httpd) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        s_httpd = NULL;
        return;
    }
    for (size_t i = 0; i < sizeof(g_handlers) / sizeof(g_handlers[0]); i++) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd, &g_handlers[i]));
    }
    ESP_LOGI(TAG, "web server up (%d endpoints, sockets=%d stack=%d softap=%d)",
             config.max_uri_handlers, config.max_open_sockets, config.stack_size,
             softap ? 1 : 0);
}
