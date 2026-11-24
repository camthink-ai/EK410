/**
 * @file evtlog.c
 * @brief Critical event log in a dedicated flash partition.
 *
 * Append-only plaintext lines. Soft cap ~48KB / 7 days (when SNTP ok).
 * Full-partition erase only on compact (over capacity) or clear — not on
 * download or every N lines. Download snapshots under the mutex then sends
 * outside the lock so slow HTTP clients do not block evtlog_event().
 */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_sntp.h"

#include "evtlog.h"

#define TAG "-->EVT"
#define PART_LABEL "evtlog"
#define HDR_MAGIC "EKEL"
#define HDR_VER 1
#define HDR_SIZE 256
#define EVTLOG_CAP_BYTES (48 * 1024)
#define EVTLOG_KEEP_SEC (7 * 24 * 3600)
#define EVTLOG_LINE_MAX (40 + EVTLOG_TAG_MAX + 1 + EVTLOG_MSG_MAX + 4)
#define FLASH_SECTOR 4096

typedef struct __attribute__((packed)) {
    char magic[4];
    uint16_t ver;
    uint16_t reserved;
    uint32_t bytes;
    uint32_t seq;
    uint8_t pad[HDR_SIZE - 4 - 2 - 2 - 4 - 4];
} evtlog_hdr_t;

_Static_assert(sizeof(evtlog_hdr_t) == HDR_SIZE, "evtlog hdr");

static const esp_partition_t *s_part;
static SemaphoreHandle_t s_mu;
static uint32_t s_bytes;
static uint32_t s_seq;
static uint32_t s_hdr_dirty; /* appends since last persist_hdr */
static bool s_ready;
static bool s_sntp_started;
static bool s_time_ok;

#define HDR_PERSIST_EVERY 8

static bool wall_time_ok(time_t *out)
{
    time_t now = time(NULL);
    if (now < 1704067200) { /* 2024-01-01 UTC */
        return false;
    }
    if (out) {
        *out = now;
    }
    return true;
}

static time_t parse_iso_line_prefix(const char *line, size_t len)
{
    if (len < 20 || line[4] != '-' || line[10] != 'T') {
        return 0;
    }
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
    if (sscanf(line, "%d-%d-%dT%d:%d:%dZ", &y, &mo, &d, &h, &mi, &s) != 6) {
        return 0;
    }
    if (y < 1970 || mo < 1 || mo > 12 || d < 1 || d > 31) {
        return 0;
    }
    static const int mdays[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int64_t days = 0;
    for (int yy = 1970; yy < y; yy++) {
        days += ((yy % 4 == 0 && (yy % 100 != 0 || yy % 400 == 0)) ? 366 : 365);
    }
    for (int m = 1; m < mo; m++) {
        days += mdays[m - 1];
        if (m == 2 && (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0))) {
            days++;
        }
    }
    days += (d - 1);
    return (time_t)(days * 86400LL + h * 3600LL + mi * 60LL + s);
}

static void format_ts(char *buf, size_t n, time_t now, uint32_t up_s)
{
    if (now > 0) {
        struct tm tm;
        gmtime_r(&now, &tm);
        snprintf(buf, n, "%04d-%02d-%02dT%02d:%02d:%02dZ up=%lu",
                 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                 tm.tm_hour, tm.tm_min, tm.tm_sec, (unsigned long)up_s);
    } else {
        snprintf(buf, n, "T+%lu", (unsigned long)up_s);
    }
}

static int load_hdr(evtlog_hdr_t *hdr)
{
    if (!s_part ||
        esp_partition_read(s_part, 0, hdr, sizeof(*hdr)) != ESP_OK) {
        return -1;
    }
    if (memcmp(hdr->magic, HDR_MAGIC, 4) != 0 || hdr->ver != HDR_VER) {
        return -1;
    }
    if (hdr->bytes > (s_part->size - HDR_SIZE)) {
        return -1;
    }
    return 0;
}

static char *malloc_buf(size_t n)
{
    char *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) {
        p = malloc(n);
    }
    return p;
}

/** Full partition rewrite — only for compact / clear / first init. */
static int rewrite_payload(const char *payload, size_t len, uint32_t seq)
{
    if (!s_part) {
        return -1;
    }
    if (len > s_part->size - HDR_SIZE) {
        len = s_part->size - HDR_SIZE;
    }
    if (esp_partition_erase_range(s_part, 0, s_part->size) != ESP_OK) {
        return -1;
    }
    evtlog_hdr_t hdr = { 0 };
    memcpy(hdr.magic, HDR_MAGIC, 4);
    hdr.ver = HDR_VER;
    hdr.bytes = (uint32_t)len;
    hdr.seq = seq;
    if (esp_partition_write(s_part, 0, &hdr, sizeof(hdr)) != ESP_OK) {
        return -1;
    }
    if (len > 0 &&
        esp_partition_write(s_part, HDR_SIZE, payload, len) != ESP_OK) {
        return -1;
    }
    s_bytes = (uint32_t)len;
    s_seq = seq;
    s_hdr_dirty = 0;
    return 0;
}

/**
 * Persist header after append without rewriting the whole partition.
 * Sector 0 holds the header + the first (FLASH_SECTOR - HDR_SIZE) body bytes.
 */
static int persist_hdr(void)
{
    if (!s_part) {
        return -1;
    }
    const size_t body_in_sec0 = FLASH_SECTOR - HDR_SIZE;
    size_t keep = s_bytes;
    if (keep > body_in_sec0) {
        keep = body_in_sec0;
    }

    uint8_t *slice = NULL;
    if (keep > 0) {
        slice = (uint8_t *)malloc_buf(keep);
        if (!slice) {
            return -1;
        }
        if (esp_partition_read(s_part, HDR_SIZE, slice, keep) != ESP_OK) {
            free(slice);
            return -1;
        }
    }

    if (esp_partition_erase_range(s_part, 0, FLASH_SECTOR) != ESP_OK) {
        free(slice);
        return -1;
    }

    evtlog_hdr_t hdr = { 0 };
    memcpy(hdr.magic, HDR_MAGIC, 4);
    hdr.ver = HDR_VER;
    hdr.bytes = s_bytes;
    hdr.seq = s_seq;
    if (esp_partition_write(s_part, 0, &hdr, sizeof(hdr)) != ESP_OK) {
        free(slice);
        return -1;
    }
    if (keep > 0 &&
        esp_partition_write(s_part, HDR_SIZE, slice, keep) != ESP_OK) {
        free(slice);
        return -1;
    }
    free(slice);
    return 0;
}

static size_t filter_payload(char *buf, size_t len, size_t cap)
{
    time_t now = 0;
    const bool age = wall_time_ok(&now);
    const time_t cut = age ? (now - (time_t)EVTLOG_KEEP_SEC) : 0;

    size_t keep_from = 0;
    if (len > cap) {
        size_t start = len - cap;
        while (start < len && buf[start] != '\n') {
            start++;
        }
        if (start < len) {
            start++;
        }
        keep_from = start;
    }

    if (age) {
        size_t line = keep_from;
        while (line < len) {
            size_t end = line;
            while (end < len && buf[end] != '\n') {
                end++;
            }
            time_t ts = parse_iso_line_prefix(buf + line, end - line);
            if (ts > 0 && ts < cut) {
                line = (end < len) ? end + 1 : end;
                keep_from = line;
                continue;
            }
            break;
        }
    }

    if (keep_from == 0) {
        return (len > cap) ? cap : len;
    }
    size_t keep = len - keep_from;
    memmove(buf, buf + keep_from, keep);
    return keep;
}

static int read_all(char **out, size_t *out_len)
{
    *out = NULL;
    *out_len = 0;
    if (s_bytes == 0) {
        return 0;
    }
    char *buf = malloc_buf(s_bytes + 8);
    if (!buf) {
        return -1;
    }
    if (esp_partition_read(s_part, HDR_SIZE, buf, s_bytes) != ESP_OK) {
        free(buf);
        return -1;
    }
    *out = buf;
    *out_len = s_bytes;
    return 0;
}

/** Compact when over capacity — full erase once. */
static int compact_with_extra(const char *extra, size_t extra_len)
{
    char *buf = NULL;
    size_t n = 0;
    if (read_all(&buf, &n) != 0) {
        return -1;
    }
    if (!buf) {
        buf = malloc_buf(extra_len + 8);
        if (!buf) {
            return -1;
        }
        n = 0;
    }
    if (extra && extra_len) {
        char *nb = malloc_buf(n + extra_len + 8);
        if (!nb) {
            free(buf);
            return -1;
        }
        if (n) {
            memcpy(nb, buf, n);
        }
        free(buf);
        buf = nb;
        memcpy(buf + n, extra, extra_len);
        n += extra_len;
    }
    n = filter_payload(buf, n, EVTLOG_CAP_BYTES);
    int rc = rewrite_payload(buf, n, s_seq + (extra_len ? 1 : 0));
    free(buf);
    return rc;
}

/**
 * After a crash, header.bytes may lag body writes. Scan past the header
 * length for non-erased flash and truncate to the last complete line.
 */
static uint32_t recover_length(uint32_t hdr_bytes)
{
    if (!s_part) {
        return 0;
    }
    const size_t max_body = s_part->size - HDR_SIZE;
    if (hdr_bytes > max_body) {
        hdr_bytes = 0;
    }

    uint8_t chunk[256];
    size_t pos = hdr_bytes;
    size_t last = hdr_bytes;
    while (pos < max_body) {
        size_t n = max_body - pos;
        if (n > sizeof(chunk)) {
            n = sizeof(chunk);
        }
        if (esp_partition_read(s_part, HDR_SIZE + pos, chunk, n) != ESP_OK) {
            break;
        }
        bool any = false;
        for (size_t i = 0; i < n; i++) {
            if (chunk[i] != 0xFF) {
                any = true;
                last = pos + i + 1;
            }
        }
        if (!any) {
            break;
        }
        pos += n;
    }

    if (last <= hdr_bytes) {
        return hdr_bytes;
    }

    /* Drop incomplete trailing line (no newline). */
    size_t scan = last;
    while (scan > hdr_bytes) {
        uint8_t b = 0;
        if (esp_partition_read(s_part, HDR_SIZE + scan - 1, &b, 1) != ESP_OK) {
            return hdr_bytes;
        }
        if (b == '\n') {
            break;
        }
        scan--;
    }
    if (scan == hdr_bytes && last > hdr_bytes) {
        /* No newline in recovered region — discard it. */
        return hdr_bytes;
    }
    return (uint32_t)scan;
}

static int append_line(const char *line, size_t line_len)
{
    if (!s_ready || !s_part || line_len == 0) {
        return -1;
    }
    if (s_bytes + line_len <= EVTLOG_CAP_BYTES &&
        HDR_SIZE + s_bytes + line_len <= s_part->size) {
        if (esp_partition_write(s_part, HDR_SIZE + s_bytes, line, line_len) != ESP_OK) {
            /* Sector may need erase mid-partition — fall back to compact. */
            return compact_with_extra(line, line_len);
        }
        s_bytes += (uint32_t)line_len;
        s_seq++;
        s_hdr_dirty++;
        if (s_hdr_dirty >= HDR_PERSIST_EVERY) {
            if (persist_hdr() != 0) {
                ESP_LOGW(TAG, "persist_hdr failed — boot recover_length will heal");
            } else {
                s_hdr_dirty = 0;
            }
        }
        return 0;
    }
    return compact_with_extra(line, line_len);
}

void evtlog_init(void)
{
    if (s_ready) {
        return;
    }
    s_mu = xSemaphoreCreateMutex();
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x41, PART_LABEL);
    if (!s_part) {
        s_part = esp_partition_find_first(ESP_PARTITION_TYPE_ANY,
                                          ESP_PARTITION_SUBTYPE_ANY, PART_LABEL);
    }
    if (!s_part) {
        ESP_LOGW(TAG, "partition '%s' missing — event log disabled", PART_LABEL);
        return;
    }

    evtlog_hdr_t hdr;
    if (load_hdr(&hdr) != 0) {
        ESP_LOGI(TAG, "init empty log on %s (%u bytes)", PART_LABEL,
                 (unsigned)s_part->size);
        s_bytes = 0;
        s_seq = 0;
        rewrite_payload("", 0, 0);
    } else {
        s_seq = hdr.seq;
        uint32_t recovered = recover_length(hdr.bytes);
        s_bytes = recovered;
        if (recovered != hdr.bytes) {
            ESP_LOGW(TAG, "recovered length %u → %u (power-loss tail)",
                     (unsigned)hdr.bytes, (unsigned)recovered);
            (void)persist_hdr();
        }
        ESP_LOGI(TAG, "loaded %u bytes seq=%u", (unsigned)s_bytes, (unsigned)s_seq);
    }
    s_ready = true;
    s_time_ok = wall_time_ok(NULL);
}

void evtlog_time_start(void)
{
    if (s_sntp_started) {
        return;
    }
    s_sntp_started = true;
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "time.cloudflare.com");
    esp_sntp_init();
    ESP_LOGI(TAG, "SNTP started for event timestamps");
}

void evtlog_event(const char *tag, const char *fmt, ...)
{
    if (!s_ready || !s_mu) {
        return;
    }
    char msg[EVTLOG_MSG_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt ? fmt : "", ap);
    va_end(ap);

    time_t now = 0;
    bool ok = wall_time_ok(&now);
    if (ok) {
        s_time_ok = true;
    }
    uint32_t up = (uint32_t)(esp_timer_get_time() / 1000000ULL);

    char ts[48];
    format_ts(ts, sizeof(ts), ok ? now : 0, up);

    char line[EVTLOG_LINE_MAX];
    const char *tg = (tag && tag[0]) ? tag : "APP";
    int n = snprintf(line, sizeof(line), "%s %.*s %s\n",
                     ts, EVTLOG_TAG_MAX, tg, msg);
    if (n <= 0) {
        return;
    }
    if ((size_t)n >= sizeof(line)) {
        n = (int)sizeof(line) - 1;
        line[n - 1] = '\n';
        line[n] = '\0';
    }

    if (xSemaphoreTake(s_mu, pdMS_TO_TICKS(2000)) != pdTRUE) {
        return;
    }
    (void)append_line(line, (size_t)n);
    xSemaphoreGive(s_mu);
}

void evtlog_get_meta(evtlog_meta_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->cap_bytes = EVTLOG_CAP_BYTES;
    out->keep_days = 7;
    out->time_ok = s_time_ok || wall_time_ok(NULL);
    if (!s_ready) {
        return;
    }
    out->present = true;
    out->bytes = s_bytes;
    out->seq = s_seq;
}

int evtlog_clear(void)
{
    if (!s_ready || !s_mu) {
        return -1;
    }
    if (xSemaphoreTake(s_mu, pdMS_TO_TICKS(5000)) != pdTRUE) {
        return -2;
    }
    int rc = rewrite_payload("", 0, s_seq + 1);
    xSemaphoreGive(s_mu);
    if (rc != 0) {
        ESP_LOGE(TAG, "event log clear failed (erase/write)");
    } else {
        ESP_LOGW(TAG, "event log cleared");
    }
    return rc;
}

int evtlog_stream(evtlog_write_fn fn, void *ctx)
{
    if (!fn) {
        return -1;
    }
    if (!s_ready || !s_mu) {
        return -1;
    }
    if (xSemaphoreTake(s_mu, pdMS_TO_TICKS(5000)) != pdTRUE) {
        return -2;
    }

    /* Snapshot under lock — no compact/flush on download. */
    char *snap = NULL;
    size_t snap_len = 0;
    int rc = 0;
    if (s_bytes > 0) {
        snap = malloc_buf(s_bytes);
        if (!snap) {
            xSemaphoreGive(s_mu);
            return -1;
        }
        if (esp_partition_read(s_part, HDR_SIZE, snap, s_bytes) != ESP_OK) {
            free(snap);
            xSemaphoreGive(s_mu);
            return -1;
        }
        snap_len = s_bytes;
    }
    xSemaphoreGive(s_mu);

    if (snap_len == 0) {
        return 0;
    }

    const size_t chunk = 1024;
    size_t off = 0;
    while (off < snap_len) {
        size_t n = snap_len - off;
        if (n > chunk) {
            n = chunk;
        }
        if (fn((const uint8_t *)snap + off, n, ctx) != 0) {
            rc = -1;
            break;
        }
        off += n;
    }
    free(snap);
    return rc;
}
