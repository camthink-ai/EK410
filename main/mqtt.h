/**
 * @file mqtt.h
 * @brief MQTT / MQTTS client for NeoMind platform integration
 *
 * Transport is explicit (mqttAttr_t.transport): mqtt:// or mqtts://.
 * NeoMind standard — one uplink + one downlink:
 *   publish  device/ek410_Eink/{deviceId}/uplink
 *   sub      device/ek410_Eink/{deviceId}/downlink
 * Uplink JSON is flat (type=telemetry|status|cmd_ack); downlink uses cmd=.
 */
#ifndef __MQTT_H__
#define __MQTT_H__

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void mqtt_open(void);
void mqtt_start(void);   /* no-op if host empty or already started */
void mqtt_stop(void);
void mqtt_restart(void);
bool mqtt_is_connected(void);
void mqtt_publish_telemetry(void);

/* Async command result (e.g. display_url finished after "queued" ack). */
void mqtt_publish_cmd_ack(const char *cmd, const char *status, const char *detail);

/* SoftAP/STA MQTT probe: ensure client up, wait for connect, publish telemetry.
 * Returns 0 on success; fills errBuf on failure. timeout_ms typically 8000. */
int mqtt_test_ping(int timeout_ms, char *errBuf, int errLen);

#ifdef __cplusplus
}
#endif

#endif /* __MQTT_H__ */
