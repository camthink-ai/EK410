/**
 * @file json_guard.c
 * @brief Depth-bounded cJSON entry points for network-supplied JSON.
 */
#include <string.h>
#include "json_guard.h"

bool json_guard_depth_ok(const char *s, size_t len, int max_depth)
{
    int depth = 0;
    bool in_str = false;
    bool esc = false;
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (in_str) {
            if (esc) {
                esc = false;
            } else if (c == '\\') {
                esc = true;
            } else if (c == '"') {
                in_str = false;
            }
            continue;
        }
        if (c == '"') {
            in_str = true;
        } else if (c == '{' || c == '[') {
            if (++depth > max_depth) {
                return false;
            }
        } else if (c == '}' || c == ']') {
            if (depth > 0) {
                depth--;
            }
        }
    }
    return true;
}

cJSON *json_guard_parse(const char *s)
{
    if (!s) {
        return NULL;
    }
    return json_guard_parse_len(s, strlen(s));
}

cJSON *json_guard_parse_len(const char *s, size_t len)
{
    if (!s || !json_guard_depth_ok(s, len, JSON_GUARD_MAX_DEPTH)) {
        return NULL;
    }
    return cJSON_ParseWithLength(s, len);
}
