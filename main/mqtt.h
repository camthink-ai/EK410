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
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void mqtt_open(void);
/** False if mutex/queue/bring-up task failed at boot — Web should show error. */
bool mqtt_infra_ok(void);
/** Last broker/TLS failure string for UI ("" if none). */
const char *mqtt_last_error(void);
void mqtt_start(void);   /* no-op if host empty or already started */
/**
 * Stop MQTT client under the module mutex.
 * Returns 0 if stopped (or already idle), -1 if mutex timed out — client
 * may still be running (e.g. large telemetry publish). Callers that tear
 * down Wi‑Fi / start NimBLE must treat -1 as failure and abort or retry.
 */
int mqtt_stop(void);
/** Retry mqtt_stop until budget_ms elapses. Returns 0 / -1. */
int mqtt_stop_wait(int budget_ms);
/** Stop then start. Returns 0, or -1 if stop timed out (old client may still run). */
int mqtt_restart(void);
bool mqtt_is_connected(void);
bool mqtt_is_started(void); /* client object exists (may still be connecting) */
/**
 * Short machine token for UI: connected | softap_paused | ble_paused |
 * no_sta | no_host | connecting | deferred_heap | disconnected.
 */
void mqtt_status_detail(char *buf, size_t len);
/** Publish telemetry uplink. Returns false if not connected / mutex / broker fail. */
bool mqtt_publish_telemetry(void);

/* Async command result (e.g. display_url finished after "queued" ack). */
void mqtt_publish_cmd_ack(const char *cmd, const char *status, const char *detail);

/* SoftAP/STA MQTT probe: ensure client up, wait for connect, publish telemetry.
 * Returns 0 on success; fills errBuf on failure. timeout_ms typically 8000. */
int mqtt_test_ping(int timeout_ms, char *errBuf, int errLen);

#ifdef __cplusplus
}
#endif

#endif /* __MQTT_H__ */
