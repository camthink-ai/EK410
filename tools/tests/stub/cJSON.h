/* Test stub standing in for IDF's cJSON: only records whether the real
 * parser was reached. main/json_guard.c is compiled against it unchanged. */
#ifndef STUB_CJSON_H
#define STUB_CJSON_H
#include <stddef.h>
typedef struct cJSON { int dummy; } cJSON;
cJSON *cJSON_ParseWithLength(const char *s, size_t len);
#endif
