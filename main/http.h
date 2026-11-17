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
bool http_server_is_up(void);

#ifdef __cplusplus
}
#endif

#endif /* __HTTP_H__ */
