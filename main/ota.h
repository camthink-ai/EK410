/**
 * @file ota.h
 * @brief HTTPS URL OTA + streamed local upload, with bootloader rollback.
 */
#ifndef __OTA_H__
#define __OTA_H__

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Download + flash the image at url (HTTPS, cert bundle). Blocking. */
int ota_start(const char *url, char *errBuf, int errLen);

/**
 * Stream a firmware image into the inactive OTA slot (web upload).
 * begin → write* → finish (sets boot partition) or abort.
 * image_len 0 = unknown size (OTA_SIZE_UNKNOWN).
 */
int ota_upload_begin(size_t image_len, char *errBuf, int errLen);
int ota_upload_write(const void *data, size_t len);
void ota_upload_abort(void);
/** Validate image + set boot partition. On success caller should reboot. */
int ota_upload_finish(char *errBuf, int errLen);
bool ota_upload_is_active(void);

/**
 * Lifecycle reported by getDevInfo. A URL OTA POST returns before the
 * download starts, so the web UI polls this to tell success from failure.
 */
typedef enum {
    OTA_STATE_IDLE = 0,
    OTA_STATE_RUNNING, /* URL download or local upload is writing flash */
    OTA_STATE_FAILED,  /* last attempt failed — ota_last_error() says why */
} ota_state_t;

/* True while flash is being written (URL download or local upload). mqtt.c
 * must not recreate its client meanwhile: OTA stops it for the heap. */
bool ota_busy(void);
/* Reserve the URL path — call BEFORE stopping MQTT so the stop's automatic
 * start-retry already sees the OTA. False if another OTA is running. */
bool ota_url_claim(void);
/* Give the URL path back; a non-empty err records a failed attempt. */
void ota_url_release(const char *err);
ota_state_t ota_state(void);
const char *ota_state_str(void); /* "idle" | "running" | "failed" */
const char *ota_last_error(void);
void ota_clear_error(void);

/* Mark the running app as valid so the bootloader keeps it (rollback guard). */
void ota_confirm_running(void);
/**
 * Pending-verify gate for a freshly booted OTA image: call when the web
 * server comes up (and once at the end of app_main). The image is confirmed
 * once the panel probe passed and the unit is reachable for recovery (web up
 * or STA online); a unit whose panel probe failed needs the web server up.
 */
void ota_selfcheck(bool web_up);

/* Running partition name + pending-rollback state for the status API. */
void ota_get_status(char *running, int runningLen, bool *pendingRollback);

/** Running app project_name from esp_app_desc (always "ek410"). */
const char *ota_running_project_name(void);

#ifdef __cplusplus
}
#endif

#endif /* __OTA_H__ */
