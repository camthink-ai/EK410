/**
 * @file json_guard.h
 * @brief Depth-bounded cJSON entry points for network-supplied JSON.
 *
 * cJSON is a recursive-descent parser: each nesting level costs ~64 bytes of
 * stack (parse_value + parse_array/parse_object, riscv32) and its own limit
 * is 1000. The callers run on small task stacks (httpd 4608/5120, mqtt_cmd
 * 5120, ct_ble 4096), so a body of nested '[' overflows the stack and
 * reboots the device. Reject deep input before the parser ever recurses.
 */
#ifndef __JSON_GUARD_H__
#define __JSON_GUARD_H__

#include <stdbool.h>
#include <stddef.h>
#include "cJSON.h"

/* Deepest legitimate payload is ~5 levels (MQTT cmd → params → items → item
 * → pos). 8 leaves margin and still fits every caller's stack. */
#define JSON_GUARD_MAX_DEPTH 8

/* True when s[0..len) nests objects/arrays no deeper than max_depth.
 * Brackets inside JSON strings (and escapes) are ignored. */
bool json_guard_depth_ok(const char *s, size_t len, int max_depth);

/* cJSON_Parse / cJSON_ParseWithLength for untrusted input. NULL when the
 * nesting is too deep or the JSON is invalid — callers treat both alike. */
cJSON *json_guard_parse(const char *s);
cJSON *json_guard_parse_len(const char *s, size_t len);

#endif /* __JSON_GUARD_H__ */
