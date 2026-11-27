/**
 * @file last_frame.c
 * @brief Flash-backed last panel frame for SoftAP / API preview.
 */
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_crc.h"
#include "mbedtls/base64.h"

#include "last_frame.h"
#include "epd709.h"
#include "mqtt.h"

#define TAG "-->LF"
#define PART_LABEL "lastfrm"
#define HDR_MAGIC "EK4F"
#define HDR_VER 1
#define HDR_SIZE 256

/* Telemetry preview: 8-bit paletted BMP. Full JSON (meta+base64) must stay
 * ≤200KB for MQTT — 400×300 → ≈121KB BMP / ≈162KB base64. */
#define PREVIEW_W 400
#define PREVIEW_H 300
#define PREVIEW_LOGICAL_W 1600
#define PREVIEW_LOGICAL_H 1200
#define PREVIEW_PALETTE_COLORS 256
#define PREVIEW_BMP_HDR (14 + 40 + PREVIEW_PALETTE_COLORS * 4)
#define PREVIEW_MQTT_JSON_MAX (200 * 1024)

/* Packed so int64 does not insert alignment holes (must be exactly HDR_SIZE). */
typedef struct __attribute__((packed)) {
    char magic[4];
    uint16_t ver;
    uint16_t reserved;
    uint32_t frame_bytes;
    uint32_t crc32;
    uint32_t seq;
    int64_t saved_us;
    char source[LAST_FRAME_SOURCE_MAX];
    uint8_t pad[HDR_SIZE - 4 - 2 - 2 - 4 - 4 - 4 - 8 - LAST_FRAME_SOURCE_MAX];
} last_frame_hdr_t;

_Static_assert(sizeof(last_frame_hdr_t) == HDR_SIZE, "last_frame hdr size");

static const esp_partition_t *s_part;
static uint8_t *s_cap;
static size_t s_cap_off;
static char s_cap_src[LAST_FRAME_SOURCE_MAX];
static bool s_capturing;
static uint32_t s_seq;
static bool s_meta_valid;
static last_frame_meta_t s_meta;
static SemaphoreHandle_t s_stream_mu;
static SemaphoreHandle_t s_preview_mu;
static char *s_preview_b64;
static int s_preview_w;
static int s_preview_h;

static void preview_lock(void)
{
    if (s_preview_mu) {
        xSemaphoreTake(s_preview_mu, portMAX_DELAY);
    }
}

static void preview_unlock(void)
{
    if (s_preview_mu) {
        xSemaphoreGive(s_preview_mu);
    }
}

#define STREAM_CHUNK 4096

static uint32_t crc_frame(const uint8_t *data, size_t len)
{
    return esp_crc32_le(0, data, len);
}

/* Spectra nibble → compact palette index (0..5). */
static uint8_t ink_to_pal(uint8_t ink)
{
    switch (ink & 0x0F) {
    case 0x1: return 1; /* white */
    case 0x2: return 2; /* yellow */
    case 0x3: return 3; /* red */
    case 0x5: return 4; /* blue */
    case 0x6: return 5; /* green */
    default:  return 0; /* black */
    }
}

static void pal_to_bgr(uint8_t idx, uint8_t *b, uint8_t *g, uint8_t *r)
{
    static const uint8_t s_pal[6][3] = {
        { 0, 0, 0 },       /* black */
        { 255, 255, 255 }, /* white */
        { 0, 220, 255 },   /* yellow BGR */
        { 30, 30, 220 },   /* red */
        { 200, 80, 30 },   /* blue */
        { 60, 160, 30 },   /* green */
    };
    if (idx > 5) {
        idx = 0;
    }
    *b = s_pal[idx][0];
    *g = s_pal[idx][1];
    *r = s_pal[idx][2];
}

static uint8_t native_ink_at(const uint8_t *frame, int nx, int ny)
{
    if (nx < 0 || ny < 0 || nx >= EPD_NATIVE_W || ny >= EPD_NATIVE_H || !frame) {
        return 0;
    }
    int csx = nx / EPD_CS_WIDTH_PX;
    int local = nx % EPD_CS_WIDTH_PX;
    size_t off = (size_t)csx * EPD_HALF_BYTES + (size_t)ny * EPD_CS_LINE_BYTES +
                 (size_t)(local / 2);
    uint8_t byte = frame[off];
    return (local & 1) ? (byte & 0x0F) : (byte >> 4);
}

static void preview_clear_unlocked(void)
{
    if (s_preview_b64) {
        free(s_preview_b64);
        s_preview_b64 = NULL;
    }
    s_preview_w = 0;
    s_preview_h = 0;
}

void last_frame_preview_drop(void)
{
    preview_lock();
    if (s_preview_b64) {
        ESP_LOGI(TAG, "drop preview to free SPIRAM for URL decode");
        preview_clear_unlocked();
    }
    preview_unlock();
}

/** Build ≤200KB 8-bit paletted BMP + base64 from native last-frame buffer. */
static void preview_rebuild_from_native(const uint8_t *frame)
{
    preview_lock();
    preview_clear_unlocked();
    preview_unlock();
    if (!frame) {
        return;
    }

    /* Width 480 is already 4-byte aligned for 8bpp rows. */
    const size_t row_stride = (size_t)PREVIEW_W;
    const size_t pixel_bytes = row_stride * PREVIEW_H;
    const size_t bmp_size = PREVIEW_BMP_HDR + pixel_bytes;
    /* base64 ≈ ceil(n/3)*4; leave headroom for telemetry JSON wrapper. */
    size_t b64_est = ((bmp_size + 2) / 3) * 4;
    if (bmp_size > 150 * 1024 || b64_est + 4096 > PREVIEW_MQTT_JSON_MAX) {
        ESP_LOGE(TAG, "preview bmp %u (b64~%u) exceeds MQTT 200KB budget",
                 (unsigned)bmp_size, (unsigned)b64_est);
        return;
    }
    uint8_t *bmp = heap_caps_malloc(bmp_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!bmp) {
        bmp = malloc(bmp_size);
    }
    if (!bmp) {
        ESP_LOGW(TAG, "preview BMP OOM");
        return;
    }
    memset(bmp, 0, bmp_size);

    /* BITMAPFILEHEADER */
    bmp[0] = 'B';
    bmp[1] = 'M';
    bmp[2] = (uint8_t)(bmp_size);
    bmp[3] = (uint8_t)(bmp_size >> 8);
    bmp[4] = (uint8_t)(bmp_size >> 16);
    bmp[5] = (uint8_t)(bmp_size >> 24);
    bmp[10] = (uint8_t)PREVIEW_BMP_HDR;
    bmp[11] = (uint8_t)(PREVIEW_BMP_HDR >> 8);
    /* BITMAPINFOHEADER */
    bmp[14] = 40;
    bmp[18] = (uint8_t)PREVIEW_W;
    bmp[19] = (uint8_t)(PREVIEW_W >> 8);
    bmp[22] = (uint8_t)PREVIEW_H;
    bmp[23] = (uint8_t)(PREVIEW_H >> 8);
    bmp[26] = 1;  /* planes */
    bmp[28] = 8;  /* 8bpp indexed */
    /* biClrUsed = 0 → full 256-entry table */

    uint8_t *pal = bmp + 54;
    for (int i = 0; i < 6; i++) {
        uint8_t b, g, r;
        pal_to_bgr((uint8_t)i, &b, &g, &r);
        pal[i * 4 + 0] = b;
        pal[i * 4 + 1] = g;
        pal[i * 4 + 2] = r;
        pal[i * 4 + 3] = 0;
    }

    for (int py = 0; py < PREVIEW_H; py++) {
        int src_y = PREVIEW_H - 1 - py; /* BMP bottom-up */
        uint8_t *row = bmp + PREVIEW_BMP_HDR + (size_t)py * row_stride;
        for (int px = 0; px < PREVIEW_W; px++) {
            int lx = (px * PREVIEW_LOGICAL_W) / PREVIEW_W;
            int ly = (src_y * PREVIEW_LOGICAL_H) / PREVIEW_H;
            if (lx >= PREVIEW_LOGICAL_W) {
                lx = PREVIEW_LOGICAL_W - 1;
            }
            if (ly >= PREVIEW_LOGICAL_H) {
                ly = PREVIEW_LOGICAL_H - 1;
            }
            int nx = EPD_NATIVE_W - 1 - ly;
            int ny = lx;
            row[px] = ink_to_pal(native_ink_at(frame, nx, ny));
        }
    }

    size_t olen = 0;
    mbedtls_base64_encode(NULL, 0, &olen, bmp, bmp_size);
    char *b64 = heap_caps_malloc(olen + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!b64) {
        b64 = malloc(olen + 1);
    }
    if (!b64) {
        free(bmp);
        ESP_LOGW(TAG, "preview b64 OOM");
        return;
    }
    size_t written = 0;
    if (mbedtls_base64_encode((unsigned char *)b64, olen + 1, &written, bmp, bmp_size) != 0) {
        free(bmp);
        free(b64);
        ESP_LOGW(TAG, "preview b64 encode fail");
        return;
    }
    b64[written] = '\0';
    free(bmp);
    preview_lock();
    preview_clear_unlocked();
    s_preview_b64 = b64;
    s_preview_w = PREVIEW_W;
    s_preview_h = PREVIEW_H;
    preview_unlock();
    ESP_LOGI(TAG, "preview ready %dx%d 8bpp bmp=%u b64=%u",
             PREVIEW_W, PREVIEW_H, (unsigned)bmp_size, (unsigned)written);
}

static int ensure_cap_buf(void)
{
    if (s_cap) {
        return 0;
    }
    s_cap = heap_caps_malloc(LAST_FRAME_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_cap) {
        s_cap = malloc(LAST_FRAME_BYTES);
    }
    return s_cap ? 0 : -1;
}

static void preview_rebuild_from_flash(void)
{
    if (!s_part || !s_meta.present) {
        last_frame_preview_drop();
        return;
    }
    if (ensure_cap_buf() != 0) {
        ESP_LOGW(TAG, "preview: no buffer");
        return;
    }
    if (esp_partition_read(s_part, HDR_SIZE, s_cap, LAST_FRAME_BYTES) != ESP_OK) {
        ESP_LOGW(TAG, "preview: flash read fail");
        return;
    }
    preview_rebuild_from_native(s_cap);
}

void last_frame_preview_restore(void)
{
    /* last_frame_preview_drop() runs before every URL fetch, and a commit
     * builds the preview again. When nothing is committed — the fetch failed,
     * the frame is the one already saved, a proxyImage only fetched —
     * telemetry would go out without a preview until the next display.
     * Reads the frame buffer: the caller holds the panel lock. */
    preview_lock();
    bool have = s_preview_b64 != NULL;
    preview_unlock();
    if (!have) {
        preview_rebuild_from_flash();
    }
}

const char *last_frame_preview_b64(void)
{
    /* Unsafe across drop/rebuild — prefer last_frame_preview_snapshot(). */
    return s_preview_b64;
}

int last_frame_preview_w(void)
{
    return s_preview_w;
}

int last_frame_preview_h(void)
{
    return s_preview_h;
}

const char *last_frame_preview_mime(void)
{
    return s_preview_b64 ? "image/bmp" : "";
}

int last_frame_preview_snapshot(last_frame_preview_snap_t *out)
{
    if (!out) {
        return -1;
    }
    out->b64 = NULL;
    out->w = 0;
    out->h = 0;
    preview_lock();
    if (s_preview_b64 && s_preview_b64[0]) {
        size_t n = strlen(s_preview_b64);
        char *copy = heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!copy) {
            copy = malloc(n + 1);
        }
        if (copy) {
            memcpy(copy, s_preview_b64, n + 1);
            out->b64 = copy;
            out->w = s_preview_w;
            out->h = s_preview_h;
        }
    }
    preview_unlock();
    return 0;
}

void last_frame_init(void)
{
    if (!s_stream_mu) {
        s_stream_mu = xSemaphoreCreateMutex();
    }
    if (!s_preview_mu) {
        s_preview_mu = xSemaphoreCreateMutex();
    }
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, PART_LABEL);
    if (!s_part) {
        ESP_LOGW(TAG, "partition '%s' missing — last-frame persist disabled", PART_LABEL);
        return;
    }
    if (s_part->size < HDR_SIZE + LAST_FRAME_BYTES) {
        ESP_LOGE(TAG, "partition too small (%u)", (unsigned)s_part->size);
        s_part = NULL;
        return;
    }
    last_frame_hdr_t hdr;
    if (esp_partition_read(s_part, 0, &hdr, sizeof(hdr)) != ESP_OK) {
        return;
    }
    if (memcmp(hdr.magic, HDR_MAGIC, 4) != 0 || hdr.ver != HDR_VER ||
        hdr.frame_bytes != LAST_FRAME_BYTES) {
        ESP_LOGI(TAG, "no valid last frame on flash");
        return;
    }
    s_seq = hdr.seq;
    s_meta.present = true;
    s_meta.seq = hdr.seq;
    s_meta.crc32 = hdr.crc32;
    s_meta.saved_us = hdr.saved_us;
    strlcpy(s_meta.source, hdr.source, sizeof(s_meta.source));
    s_meta_valid = true;
    ESP_LOGI(TAG, "last frame present seq=%u src=%s", (unsigned)s_seq,
             s_meta.source[0] ? s_meta.source : "-");
    preview_rebuild_from_flash();
    /* Lean telemetry only attaches preview when s_previewDue is set. Arm once
     * after boot so the first heartbeat can show the restored frame; later
     * uploads call mqtt_note_frame_changed() themselves. */
    mqtt_note_frame_changed();
}

int last_frame_capture_begin(const char *source)
{
    if (!s_part) {
        return -1;
    }
    if (ensure_cap_buf() != 0) {
        ESP_LOGE(TAG, "capture OOM");
        return -1;
    }
    s_cap_off = 0;
    memset(s_cap_src, 0, sizeof(s_cap_src));
    if (source && source[0]) {
        strlcpy(s_cap_src, source, sizeof(s_cap_src));
    } else {
        strlcpy(s_cap_src, "panel", sizeof(s_cap_src));
    }
    s_capturing = true;
    return 0;
}

int last_frame_capture_write(const uint8_t *data, size_t len)
{
    if (!s_capturing || !s_cap || !data || len == 0) {
        return -1;
    }
    if (s_cap_off + len > LAST_FRAME_BYTES) {
        ESP_LOGW(TAG, "capture overflow");
        return -1;
    }
    memcpy(s_cap + s_cap_off, data, len);
    s_cap_off += len;
    return 0;
}

bool last_frame_capturing(void)
{
    return s_capturing;
}

bool last_frame_capture_active(void)
{
    return s_capturing && s_cap_off > 0;
}

void last_frame_capture_abort(void)
{
    s_cap_off = 0;
    s_cap_src[0] = '\0';
    s_capturing = false;
}

uint8_t *last_frame_buffer(void)
{
    if (!s_part || ensure_cap_buf() != 0) {
        return NULL;
    }
    return s_cap;
}

/* s_cap holds a complete frame, s_cap_src its source: flash, then preview. */
static int commit_frame(void)
{
    last_frame_hdr_t hdr = {0};
    memcpy(hdr.magic, HDR_MAGIC, 4);
    hdr.ver = HDR_VER;
    hdr.frame_bytes = LAST_FRAME_BYTES;
    hdr.crc32 = crc_frame(s_cap, LAST_FRAME_BYTES);
    /* Every commit erases the whole 1MB partition (no wear levelling — one
     * erase cycle per sector per refresh): skip it for an identical frame. */
    if (s_meta_valid && s_meta.present && s_meta.crc32 == hdr.crc32) {
        ESP_LOGI(TAG, "last frame unchanged (crc=0x%08lx) — flash write skipped",
                 (unsigned long)hdr.crc32);
        /* The fetch dropped the preview; s_cap holds the same frame as flash. */
        preview_rebuild_from_native(s_cap);
        mqtt_note_frame_changed();
        last_frame_capture_abort();
        return 0;
    }
    hdr.seq = s_seq + 1;
    hdr.saved_us = esp_timer_get_time();
    strlcpy(hdr.source, s_cap_src, sizeof(hdr.source));

    /* Erase → body → header last. Power loss mid-body leaves no valid magic
     * hdr (or stale erased flash), so boot will not replay a truncated frame. */
    esp_err_t e = esp_partition_erase_range(s_part, 0, s_part->size);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "erase fail %s", esp_err_to_name(e));
        s_meta_valid = false;
        s_meta.present = false;
        last_frame_capture_abort();
        return -1;
    }
    e = esp_partition_write(s_part, HDR_SIZE, s_cap, LAST_FRAME_BYTES);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "frame write fail %s", esp_err_to_name(e));
        s_meta_valid = false;
        s_meta.present = false;
        last_frame_capture_abort();
        return -1;
    }
    e = esp_partition_write(s_part, 0, &hdr, sizeof(hdr));
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "hdr write fail %s", esp_err_to_name(e));
        s_meta_valid = false;
        s_meta.present = false;
        last_frame_capture_abort();
        return -1;
    }

    s_seq = hdr.seq;
    s_meta.present = true;
    s_meta.seq = hdr.seq;
    s_meta.crc32 = hdr.crc32;
    s_meta.saved_us = hdr.saved_us;
    strlcpy(s_meta.source, hdr.source, sizeof(s_meta.source));
    s_meta_valid = true;
    ESP_LOGI(TAG, "saved last frame seq=%u src=%s crc=0x%08lx",
             (unsigned)s_seq, s_meta.source, (unsigned long)s_meta.crc32);
    preview_rebuild_from_native(s_cap);
    mqtt_note_frame_changed();
    last_frame_capture_abort();
    return 0;
}

int last_frame_commit_native(const char *source)
{
    if (!s_part || !s_cap) {
        return -1;
    }
    strlcpy(s_cap_src, (source && source[0]) ? source : "panel", sizeof(s_cap_src));
    return commit_frame();
}

int last_frame_capture_commit(void)
{
    if (!s_part || !s_cap || !s_capturing) {
        return -1;
    }
    if (s_cap_off != LAST_FRAME_BYTES) {
        ESP_LOGW(TAG, "capture incomplete %u/%u — skip save",
                 (unsigned)s_cap_off, (unsigned)LAST_FRAME_BYTES);
        last_frame_capture_abort();
        return -1;
    }
    return commit_frame();
}

int last_frame_get_meta(last_frame_meta_t *out)
{
    if (!out) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    if (!s_part) {
        return -1;
    }
    if (s_meta_valid) {
        *out = s_meta;
        return 0;
    }
    last_frame_init(); /* re-read */
    if (s_meta_valid) {
        *out = s_meta;
        return 0;
    }
    return -1;
}

int last_frame_read(uint8_t *out, size_t out_len)
{
    if (!s_part || !out || out_len < LAST_FRAME_BYTES) {
        return -1;
    }
    last_frame_hdr_t hdr;
    if (esp_partition_read(s_part, 0, &hdr, sizeof(hdr)) != ESP_OK) {
        return -1;
    }
    if (memcmp(hdr.magic, HDR_MAGIC, 4) != 0 || hdr.ver != HDR_VER ||
        hdr.frame_bytes != LAST_FRAME_BYTES) {
        return -1;
    }
    if (esp_partition_read(s_part, HDR_SIZE, out, LAST_FRAME_BYTES) != ESP_OK) {
        return -1;
    }
    uint32_t crc = crc_frame(out, LAST_FRAME_BYTES);
    if (crc != hdr.crc32) {
        ESP_LOGW(TAG, "crc mismatch 0x%08lx vs 0x%08lx",
                 (unsigned long)crc, (unsigned long)hdr.crc32);
        return -1;
    }
    return 0;
}

int last_frame_stream(last_frame_write_fn write_fn, void *ctx)
{
    if (!s_part || !write_fn || !s_stream_mu) {
        return -1;
    }
    /* Non-blocking: concurrent SoftAP fetches must not pile up on one worker. */
    if (xSemaphoreTake(s_stream_mu, 0) != pdTRUE) {
        return -2;
    }

    int rc = -1;
    last_frame_hdr_t hdr;
    if (esp_partition_read(s_part, 0, &hdr, sizeof(hdr)) != ESP_OK) {
        goto out;
    }
    if (memcmp(hdr.magic, HDR_MAGIC, 4) != 0 || hdr.ver != HDR_VER ||
        hdr.frame_bytes != LAST_FRAME_BYTES) {
        goto out;
    }

    /* DMA-capable: panel SPI must not fall back to priv TX alloc under Wi-Fi. */
    uint8_t *buf = heap_caps_malloc(STREAM_CHUNK, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!buf) {
        buf = heap_caps_malloc(STREAM_CHUNK, MALLOC_CAP_DMA);
    }
    if (!buf) {
        buf = malloc(STREAM_CHUNK);
    }
    if (!buf) {
        ESP_LOGE(TAG, "stream OOM");
        goto out;
    }

    /* Pass 1: CRC before any consumer write — avoid painting a corrupt frame. */
    uint32_t crc = 0;
    size_t off = 0;
    while (off < LAST_FRAME_BYTES) {
        size_t n = LAST_FRAME_BYTES - off;
        if (n > STREAM_CHUNK) {
            n = STREAM_CHUNK;
        }
        if (esp_partition_read(s_part, HDR_SIZE + off, buf, n) != ESP_OK) {
            free(buf);
            goto out;
        }
        crc = esp_crc32_le(crc, buf, n);
        off += n;
        if ((off & 0x7FFF) == 0) {
            vTaskDelay(1);
        }
    }
    if (crc != hdr.crc32) {
        ESP_LOGW(TAG, "stream crc mismatch 0x%08lx vs hdr 0x%08lx — discard",
                 (unsigned long)crc, (unsigned long)hdr.crc32);
        s_meta.present = false;
        s_meta_valid = false;
        free(buf);
        goto out;
    }

    /* Pass 2: deliver to caller (panel upload / HTTP download). */
    off = 0;
    while (off < LAST_FRAME_BYTES) {
        size_t n = LAST_FRAME_BYTES - off;
        if (n > STREAM_CHUNK) {
            n = STREAM_CHUNK;
        }
        if (esp_partition_read(s_part, HDR_SIZE + off, buf, n) != ESP_OK) {
            free(buf);
            goto out;
        }
        if (write_fn(buf, n, ctx) != 0) {
            free(buf);
            goto out;
        }
        off += n;
        /* Yield so Wi-Fi / httpd can service SoftAP SPA while streaming ~960KB. */
        if ((off & 0x7FFF) == 0) {
            vTaskDelay(1);
        }
    }
    free(buf);
    rc = 0;

out:
    xSemaphoreGive(s_stream_mu);
    return rc;
}
