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

/* Mark the running app as valid so the bootloader keeps it (rollback guard). */
void ota_confirm_running(void);

/* Running partition name + pending-rollback state for the status API. */
void ota_get_status(char *running, int runningLen, bool *pendingRollback);

/** Running app project_name from esp_app_desc (always "ek410"). */
const char *ota_running_project_name(void);

#ifdef __cplusplus
}
#endif

#endif /* __OTA_H__ */
