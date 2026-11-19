/**
 * @file http.h
 * @brief Web configuration server (REST API + embedded page)
 */
#ifndef __HTTP_H__
#define __HTTP_H__

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void http_server_start(void);
void http_server_stop(void);
/** Arm async stop and wait up to budget_ms for the old server to finish. */
int http_server_stop_wait(int budget_ms);
bool http_server_is_up(void);
/** True while an async httpd_stop is still running (start must wait). */
bool http_server_is_stopping(void);

#ifdef __cplusplus
}
#endif

#endif /* __HTTP_H__ */
