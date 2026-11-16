/**
 * @file ota.h
 * @brief HTTPS OTA upgrade with automatic rollback
 */
#ifndef __OTA_H__
#define __OTA_H__

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Download + flash the image at url (HTTPS, cert bundle). Blocking. */
int ota_start(const char *url, char *errBuf, int errLen);

/* Mark the running app as valid so the bootloader keeps it (rollback guard). */
void ota_confirm_running(void);

/* Running partition name + pending-rollback state for the status API. */
void ota_get_status(char *running, int runningLen, bool *pendingRollback);

#ifdef __cplusplus
}
#endif

#endif /* __OTA_H__ */
