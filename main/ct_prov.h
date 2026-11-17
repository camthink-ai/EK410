/**
 * @file ct_prov.h
 * @brief CamThink CT-Prov/1 SoftAP + BLE dispatcher for NeoLink.
 *
 * SoftAP binding (protocol §16): POST /api/v1/ctprov with one whole JSON
 * envelope per HTTP round trip. BLE GATT uses the same JSON ops over the
 * fragment layer (§5). Ops: device.info.get, communication.*,
 * config.set / config.get, session.close.
 */
#ifndef CT_PROV_H_
#define CT_PROV_H_

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Model wire name used in device.info / discovery. */
#define CT_PROV_MODEL_NAME "EK410"

/** BLE advertising model_code (unknown to NeoLink is still connectable). */
#define CT_PROV_MODEL_CODE 0x03

/** HTTP POST /api/v1/ctprov handler. */
esp_err_t ct_prov_http_handle(httpd_req_t *req);

/**
 * SoftAP SSID for NeoLink AP discovery (legacy form EK410_<MAC6>).
 * MAC6 = last 3 bytes of SoftAP MAC, uppercase hex.
 */
void ct_prov_softap_ssid(char *buf, int buflen);

/**
 * BLE Complete Local Name / META name: EK410-<MAC6> (hyphen; A-Z0-9- only).
 * MAC6 = last 3 bytes of SoftAP MAC, uppercase hex.
 */
void ct_prov_ble_name(char *buf, int buflen);

/**
 * Process one CT-Prov JSON request body (no BOM).
 * @param body UTF-8 JSON request (not necessarily NUL-terminated if len set)
 * @param len  byte length of body
 * @param out_json malloc'd response JSON on success / BUSY; caller frees
 * @return ESP_OK with *out_json set;
 *         ESP_ERR_INVALID_ARG for malformed envelope (*out_json NULL);
 *         ESP_ERR_NO_MEM on OOM;
 *         ESP_ERR_INVALID_STATE when another request holds the busy lock
 *         (still returns a BUSY JSON in *out_json when possible).
 */
esp_err_t ct_prov_handle_json(const char *body, int len, char **out_json);

#ifdef __cplusplus
}
#endif

#endif /* CT_PROV_H_ */
