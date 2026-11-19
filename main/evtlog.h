/**
 * @file evtlog.h
 * @brief Flash-backed critical event log (7-day / size capped) for field debug.
 */
#ifndef EVTLOG_H_
#define EVTLOG_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EVTLOG_TAG_MAX 12
#define EVTLOG_MSG_MAX 160

typedef struct {
    bool present;
    uint32_t bytes;
    uint32_t seq;
    uint32_t cap_bytes;
    int keep_days;
    bool time_ok; /* wall clock synced (SNTP) — enables 7-day purge */
} evtlog_meta_t;

void evtlog_init(void);

/** Append one critical event. tag e.g. WIFI/MQTT/BLE/EPD/SLP/BOOT. */
void evtlog_event(const char *tag, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

void evtlog_get_meta(evtlog_meta_t *out);
void evtlog_clear(void);

/** Start SNTP once STA has IP (idempotent). Improves timestamps + 7-day trim. */
void evtlog_time_start(void);

typedef int (*evtlog_write_fn)(const uint8_t *data, size_t len, void *ctx);

/** Stream plaintext log to callback (chunked). 0=ok, -1=fail, -2=busy. */
int evtlog_stream(evtlog_write_fn fn, void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* EVTLOG_H_ */
