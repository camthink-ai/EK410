/**
 * @file config.c
 * @brief Device configuration persisted in NVS
 *
 * First boot (empty NVS) burns a random 8-hex deviceId used for:
 *   MQTT clientId = ek410-{deviceId}
 *   SoftAP SSID   = EK410-{deviceId}
 *   display name  = EK410-{deviceId}
 * NeoMind topics: device/ek410_Eink/{deviceId}/uplink|downlink
 */
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_mac.h"
#include "esp_random.h"

#include "config.h"
#include "version.h"

#define TAG "-->CFG"
#define NS "epcfg"

static devInfo_t g_devInfo = {0};

static void ensure_device_identity(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }

    char uid[17] = {0};
    size_t len = sizeof(uid);
    bool fresh = false;
    if (nvs_get_str(h, "uid", uid, &len) != ESP_OK || uid[0] == '\0') {
        uint8_t r[4];
        esp_fill_random(r, sizeof(r));
        snprintf(uid, sizeof(uid), "%02x%02x%02x%02x", r[0], r[1], r[2], r[3]);
        nvs_set_str(h, "uid", uid);
        fresh = true;
    }
    strlcpy(g_devInfo.deviceId, uid, sizeof(g_devInfo.deviceId));

    char name[33] = {0};
    len = sizeof(name);
    if (nvs_get_str(h, "name", name, &len) != ESP_OK || name[0] == '\0') {
        snprintf(name, sizeof(name), "EK410-%s", uid);
        nvs_set_str(h, "name", name);
        fresh = true;
    }

    char cid[64] = {0};
    len = sizeof(cid);
    if (nvs_get_str(h, "cid", cid, &len) != ESP_OK || cid[0] == '\0') {
        /* MQTT client id; NeoMind topics use deviceId, not clientId */
        snprintf(cid, sizeof(cid), "ek410-%s", uid);
        nvs_set_str(h, "cid", cid);
        fresh = true;
    }

    /* Default MQTT port/qos/transport; host left empty until broker is set. */
    int32_t port = 0;
    if (nvs_get_i32(h, "port", &port) != ESP_OK) {
        nvs_set_i32(h, "port", 1883);
        fresh = true;
    }
    int32_t qos = 0;
    if (nvs_get_i32(h, "qos", &qos) != ESP_OK) {
        nvs_set_i32(h, "qos", 1);
        fresh = true;
    }
    int32_t transport = 0;
    if (nvs_get_i32(h, "tls", &transport) != ESP_OK) {
        nvs_set_i32(h, "tls", MQTT_TRANSPORT_TCP);
        fresh = true;
    }

    if (fresh) {
        nvs_commit(h);
        ESP_LOGI(TAG, "device identity burned: uid=%s name=%s cid=%s", uid, name, cid);
    }
    nvs_close(h);
}

void config_open(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(g_devInfo.mac, sizeof(g_devInfo.mac), "%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    strlcpy(g_devInfo.fwVersion, FW_VERSION, sizeof(g_devInfo.fwVersion));
    strlcpy(g_devInfo.panel, "EK410 7.09\" 1600x1200 6c", sizeof(g_devInfo.panel));
    ensure_device_identity();
}

bool config_has_wifi(void)
{
    wifiAttr_t wifi;
    cfg_get_wifi_attr(&wifi);
    return wifi.ssid[0] != '\0';
}

bool config_is_provisioned(void)
{
    /* NeoMind-ready: home Wi-Fi + broker. STA starts on Wi-Fi alone. */
    mqttAttr_t mqtt;
    cfg_get_mqtt_attr(&mqtt);
    return config_has_wifi() && mqtt.host[0] != '\0';
}

void cfg_get_wifi_attr(wifiAttr_t *wifi)
{
    nvs_handle_t h;
    memset(wifi, 0, sizeof(*wifi));
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    size_t len = sizeof(wifi->ssid);
    nvs_get_str(h, "ssid", wifi->ssid, &len);
    len = sizeof(wifi->password);
    nvs_get_str(h, "pass", wifi->password, &len);
    nvs_close(h);
}

void cfg_set_wifi_attr(const wifiAttr_t *wifi)
{
    nvs_handle_t h;
    ESP_ERROR_CHECK(nvs_open(NS, NVS_READWRITE, &h));
    ESP_ERROR_CHECK(nvs_set_str(h, "ssid", wifi->ssid));
    ESP_ERROR_CHECK(nvs_set_str(h, "pass", wifi->password));
    ESP_ERROR_CHECK(nvs_commit(h));
    nvs_close(h);
    ESP_LOGI(TAG, "wifi attr saved (ssid=%s)", wifi->ssid);
}

const char *mqtt_transport_str(int transport)
{
    return (transport == MQTT_TRANSPORT_TLS) ? "mqtts" : "mqtt";
}

int mqtt_transport_from_str(const char *s)
{
    if (!s || !s[0]) {
        return MQTT_TRANSPORT_TCP;
    }
    if (!strcasecmp(s, "mqtts") || !strcasecmp(s, "tls") || !strcasecmp(s, "ssl")) {
        return MQTT_TRANSPORT_TLS;
    }
    return MQTT_TRANSPORT_TCP;
}

int mqtt_default_port(int transport)
{
    return (transport == MQTT_TRANSPORT_TLS) ? 8883 : 1883;
}

void cfg_get_mqtt_attr(mqttAttr_t *mqtt)
{
    nvs_handle_t h;
    memset(mqtt, 0, sizeof(*mqtt));
    mqtt->port = 1883;
    mqtt->qos = 1;
    mqtt->transport = MQTT_TRANSPORT_TCP;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    size_t len = sizeof(mqtt->host);
    nvs_get_str(h, "host", mqtt->host, &len);
    len = sizeof(mqtt->clientId);
    nvs_get_str(h, "cid", mqtt->clientId, &len);
    int32_t v = 0;
    if (nvs_get_i32(h, "port", &v) == ESP_OK) {
        mqtt->port = (int)v;
    }
    if (nvs_get_i32(h, "qos", &v) == ESP_OK) {
        mqtt->qos = (int)v;
    }
    if (nvs_get_i32(h, "tls", &v) == ESP_OK) {
        mqtt->transport = (v == MQTT_TRANSPORT_TLS) ? MQTT_TRANSPORT_TLS : MQTT_TRANSPORT_TCP;
    }
    len = sizeof(mqtt->username);
    nvs_get_str(h, "mquser", mqtt->username, &len);
    len = sizeof(mqtt->password);
    nvs_get_str(h, "mqpass", mqtt->password, &len);
    nvs_close(h);

    if (mqtt->clientId[0] == '\0') {
        if (g_devInfo.deviceId[0]) {
            snprintf(mqtt->clientId, sizeof(mqtt->clientId), "ek410-%s", g_devInfo.deviceId);
        } else {
            snprintf(mqtt->clientId, sizeof(mqtt->clientId), "ek410-%s", g_devInfo.mac);
        }
    }
    if (mqtt->port <= 0) {
        mqtt->port = mqtt_default_port(mqtt->transport);
    }
    if (mqtt->qos < 0 || mqtt->qos > 2) {
        mqtt->qos = 1;
    }
}

void cfg_set_mqtt_attr(const mqttAttr_t *mqtt)
{
    nvs_handle_t h;
    int transport = (mqtt->transport == MQTT_TRANSPORT_TLS) ? MQTT_TRANSPORT_TLS : MQTT_TRANSPORT_TCP;
    int port = mqtt->port > 0 ? mqtt->port : mqtt_default_port(transport);
    ESP_ERROR_CHECK(nvs_open(NS, NVS_READWRITE, &h));
    ESP_ERROR_CHECK(nvs_set_str(h, "host", mqtt->host));
    ESP_ERROR_CHECK(nvs_set_str(h, "cid", mqtt->clientId));
    ESP_ERROR_CHECK(nvs_set_i32(h, "port", (int32_t)port));
    ESP_ERROR_CHECK(nvs_set_i32(h, "qos", (int32_t)mqtt->qos));
    ESP_ERROR_CHECK(nvs_set_i32(h, "tls", (int32_t)transport));
    ESP_ERROR_CHECK(nvs_set_str(h, "mquser", mqtt->username));
    ESP_ERROR_CHECK(nvs_set_str(h, "mqpass", mqtt->password));
    ESP_ERROR_CHECK(nvs_commit(h));
    nvs_close(h);
    ESP_LOGI(TAG, "mqtt attr saved (%s://%s:%d)", mqtt_transport_str(transport), mqtt->host, port);
}

void cfg_get_dev_info(devInfo_t *info)
{
    nvs_handle_t h;
    *info = g_devInfo;
    if (info->deviceId[0] == '\0') {
        /* Reload uid if config_open path skipped somehow. */
        if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
            size_t len = sizeof(info->deviceId);
            nvs_get_str(h, "uid", info->deviceId, &len);
            nvs_close(h);
        }
    }
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(info->devName);
        if (nvs_get_str(h, "name", info->devName, &len) != ESP_OK || info->devName[0] == '\0') {
            if (info->deviceId[0]) {
                snprintf(info->devName, sizeof(info->devName), "EK410-%s", info->deviceId);
            } else {
                snprintf(info->devName, sizeof(info->devName), "EK410-%s", info->mac + 6);
            }
        }
        nvs_close(h);
    } else if (info->devName[0] == '\0') {
        snprintf(info->devName, sizeof(info->devName), "EK410-%s",
                 info->deviceId[0] ? info->deviceId : info->mac + 6);
    }
}

void cfg_set_dev_name(const char *name)
{
    nvs_handle_t h;
    ESP_ERROR_CHECK(nvs_open(NS, NVS_READWRITE, &h));
    ESP_ERROR_CHECK(nvs_set_str(h, "name", name));
    ESP_ERROR_CHECK(nvs_commit(h));
    nvs_close(h);
    ESP_LOGI(TAG, "dev name saved (%s)", name);
}

const char *power_mode_str(int mode)
{
    switch (mode) {
    case POWER_MODE_CYCLE:
        return "cycle";
    case POWER_MODE_HIBERNATE:
        return "hibernate";
    default:
        return "always";
    }
}

int power_mode_from_str(const char *s)
{
    if (!s || !s[0]) {
        return POWER_MODE_ALWAYS;
    }
    if (!strcasecmp(s, "cycle") || !strcasecmp(s, "timed")) {
        return POWER_MODE_CYCLE;
    }
    if (!strcasecmp(s, "hibernate") || !strcasecmp(s, "sleep") || !strcasecmp(s, "off")) {
        return POWER_MODE_HIBERNATE;
    }
    return POWER_MODE_ALWAYS;
}

const char *img_quality_str(int q)
{
    return (q == IMG_QUALITY_SPEED) ? "speed" : "quality";
}

int img_quality_from_str(const char *s)
{
    if (s && (!strcasecmp(s, "speed") || !strcasecmp(s, "fast"))) {
        return IMG_QUALITY_SPEED;
    }
    return IMG_QUALITY_QUALITY;
}

void cfg_get_power_attr(powerAttr_t *p)
{
    nvs_handle_t h;
    memset(p, 0, sizeof(*p));
    p->mode = POWER_MODE_ALWAYS;
    p->sleepMin = 0;
    p->batteryAdcGpio = 0;
    p->batteryDivMv = 4200;
    p->ledGpio = 0;
    p->imageQuality = IMG_QUALITY_QUALITY;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    int32_t v = 0;
    if (nvs_get_i32(h, "sleepmin", &v) == ESP_OK) {
        p->sleepMin = (int)v;
    }
    if (nvs_get_i32(h, "pwrmode", &v) == ESP_OK) {
        if (v == POWER_MODE_CYCLE || v == POWER_MODE_HIBERNATE) {
            p->mode = (int)v;
        } else {
            p->mode = POWER_MODE_ALWAYS;
        }
    } else if (p->sleepMin > 0) {
        /* Legacy NVS: sleepMin alone meant timed cycle. */
        p->mode = POWER_MODE_CYCLE;
    }
    if (nvs_get_i32(h, "batadc", &v) == ESP_OK) {
        p->batteryAdcGpio = (int)v;
    }
    if (nvs_get_i32(h, "batmv", &v) == ESP_OK && v > 0) {
        p->batteryDivMv = (int)v;
    }
    if (nvs_get_i32(h, "ledgpio", &v) == ESP_OK) {
        p->ledGpio = (int)v;
    }
    if (nvs_get_i32(h, "imgqual", &v) == ESP_OK) {
        p->imageQuality = (v == IMG_QUALITY_SPEED) ? IMG_QUALITY_SPEED : IMG_QUALITY_QUALITY;
    }
    size_t len = sizeof(p->wakeUrl);
    nvs_get_str(h, "wakeurl", p->wakeUrl, &len);
    nvs_close(h);
}

void cfg_set_power_attr(const powerAttr_t *p)
{
    nvs_handle_t h;
    int mode = p->mode;
    if (mode != POWER_MODE_CYCLE && mode != POWER_MODE_HIBERNATE) {
        mode = POWER_MODE_ALWAYS;
    }
    int sleepMin = p->sleepMin < 0 ? 0 : p->sleepMin;
    if (mode == POWER_MODE_CYCLE && sleepMin < 1) {
        sleepMin = 60;
    }
    int imageQuality =
        (p->imageQuality == IMG_QUALITY_SPEED) ? IMG_QUALITY_SPEED : IMG_QUALITY_QUALITY;
    ESP_ERROR_CHECK(nvs_open(NS, NVS_READWRITE, &h));
    ESP_ERROR_CHECK(nvs_set_i32(h, "pwrmode", (int32_t)mode));
    ESP_ERROR_CHECK(nvs_set_i32(h, "sleepmin", (int32_t)sleepMin));
    ESP_ERROR_CHECK(nvs_set_i32(h, "batadc", (int32_t)p->batteryAdcGpio));
    ESP_ERROR_CHECK(nvs_set_i32(h, "batmv", (int32_t)p->batteryDivMv));
    ESP_ERROR_CHECK(nvs_set_i32(h, "ledgpio", (int32_t)p->ledGpio));
    ESP_ERROR_CHECK(nvs_set_i32(h, "imgqual", (int32_t)imageQuality));
    ESP_ERROR_CHECK(nvs_set_str(h, "wakeurl", p->wakeUrl));
    ESP_ERROR_CHECK(nvs_commit(h));
    nvs_close(h);
    ESP_LOGI(TAG, "power attr saved (mode=%s, sleep=%dmin, img=%s, wakeUrl=%s)",
             power_mode_str(mode), sleepMin, img_quality_str(imageQuality),
             p->wakeUrl[0] ? p->wakeUrl : "-");
}

void cfg_factory_reset(void)
{
    /* Keep burned device identity so SoftAP / NeoMind deviceId stay stable. */
    char uid[17] = {0};
    char cid[64] = {0};
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
        size_t len = sizeof(uid);
        nvs_get_str(h, "uid", uid, &len);
        len = sizeof(cid);
        nvs_get_str(h, "cid", cid, &len);
        nvs_erase_all(h);
        if (uid[0] != '\0') {
            nvs_set_str(h, "uid", uid);
        }
        if (cid[0] != '\0') {
            nvs_set_str(h, "cid", cid);
        } else if (uid[0] != '\0') {
            char rebuilt[64];
            snprintf(rebuilt, sizeof(rebuilt), "ek410-%s", uid);
            nvs_set_str(h, "cid", rebuilt);
        }
        nvs_set_i32(h, "port", 1883);
        nvs_set_i32(h, "qos", 1);
        nvs_set_i32(h, "tls", MQTT_TRANSPORT_TCP);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGW(TAG, "factory reset done (uid preserved)");
}
