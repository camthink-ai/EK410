/**
 * @file config.h
 * @brief Device configuration (NVS-backed), CamThink house style
 */
#ifndef __CONFIG_H__
#define __CONFIG_H__

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char ssid[33];
    char password[65];
} wifiAttr_t;

/* Broker wire protocol — explicit, never inferred from port alone. */
typedef enum {
    MQTT_TRANSPORT_TCP = 0, /* mqtt://  plain TCP */
    MQTT_TRANSPORT_TLS = 1, /* mqtts:// TLS + system CA bundle */
} mqtt_transport_t;

typedef struct {
    char host[64];
    int port;
    int transport; /* mqtt_transport_t */
    char clientId[64];
    int qos;
    char username[65];
    char password[65];
} mqttAttr_t;

typedef struct {
    char devName[33];
    char mac[13];             /* STA MAC, 12 hex lowercase, no separators */
    char deviceId[17];        /* 8 hex random uid, burned on first boot */
    char fwVersion[16];
    char panel[24];
} devInfo_t;

/* Power / sleep policy (NVS). Default = always-on. */
typedef enum {
    POWER_MODE_ALWAYS = 0,      /* stay awake; sleep_now = until MAIN */
    POWER_MODE_CYCLE = 1,       /* wake window then deep-sleep sleepMin minutes */
    POWER_MODE_HIBERNATE = 2,   /* sleep-until-MAIN (off semantics); no auto cycle */
} power_mode_t;

/* On-device URL image decode (display_url / wakeUrl). SoftAP uploadFrame is
 * unaffected — the browser already does full-panel dither. */
typedef enum {
    IMG_QUALITY_SPEED = 0,      /* smaller RGB budget, faster, more grain */
    IMG_QUALITY_QUALITY = 1,    /* larger budget, slower, cleaner (default) */
} img_quality_t;

typedef struct {
    int mode;                   /* power_mode_t */
    int sleepMin;               /* cycle period minutes (used when mode==CYCLE) */
    char wakeUrl[192];          /* optional frame URL rendered after each wake */
    int batteryAdcGpio;         /* 0 = unknown/disabled; else ADC1 GPIO to sample */
    int batteryDivMv;           /* mv full-scale after divider (e.g. 4200) */
    int ledGpio;                /* legacy web field; RGB is on AW9523B */
    int imageQuality;           /* img_quality_t — default QUALITY */
} powerAttr_t;

/* Mode helpers (string <-> enum for HTTP/MQTT). */
const char *power_mode_str(int mode);
int power_mode_from_str(const char *s); /* unknown -> ALWAYS */

const char *img_quality_str(int q);     /* "speed" | "quality" */
int img_quality_from_str(const char *s); /* unknown -> QUALITY */

const char *mqtt_transport_str(int transport);       /* "mqtt" | "mqtts" */
int mqtt_transport_from_str(const char *s);          /* unknown -> TCP */
int mqtt_default_port(int transport);                /* 1883 / 8883 */

/**
 * @brief Open configuration storage (NVS init + defaults).
 */
void config_open(void);

bool config_has_wifi(void);       /* NVS has home SSID — enough to start STA */
bool config_is_provisioned(void); /* Wi-Fi + MQTT host (NeoMind-ready) */

void cfg_get_wifi_attr(wifiAttr_t *wifi);
void cfg_set_wifi_attr(const wifiAttr_t *wifi);

void cfg_get_mqtt_attr(mqttAttr_t *mqtt);
void cfg_set_mqtt_attr(const mqttAttr_t *mqtt);

void cfg_get_dev_info(devInfo_t *info);
void cfg_set_dev_name(const char *name);

void cfg_get_power_attr(powerAttr_t *p);
void cfg_set_power_attr(const powerAttr_t *p);

void cfg_factory_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* __CONFIG_H__ */
