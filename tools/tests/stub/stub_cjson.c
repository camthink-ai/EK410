#include "cJSON.h"
int g_parse_calls = 0;
cJSON *cJSON_ParseWithLength(const char *s, size_t len)
{
    (void)s;
    (void)len;
    g_parse_calls++;
    return (cJSON *)1;
}
