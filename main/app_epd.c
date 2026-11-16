/*
 * app_epd.c — streaming display layer over the ported EPD709 driver.
 *
 * The full frame is 960000 bytes (two 480000-byte halves), which does not fit
 * in ESP32-C6 RAM next to the WiFi/MQTT stacks. Everything here therefore
 * streams: a DTM command is opened on one driver IC, 600-byte lines (or larger
 * chunks from HTTP) are pushed through a small DMA buffer, and CS is released
 * only when the half is complete.
 * Native panel is 1200x1600 with CS0/CS1 = left/right 600 px (VE071A BSP).
 * UI is landscape 1600x1200 via 90° CCW: logical (lx,ly) <-> native
 *   lx = ny, ly = NATIVE_W-1-nx.
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_timer.h"

#include "app_epd.h"
#include "canvas.h"
#include "EPD709.h"
#include "comm.h"
#include "pindefine.h"
#include "status.h"
#include "font8x16.h"
#include "frame_data.h"
#include "board_ioext.h"
#include "splash.h"
#include "img_decode.h"
#include "mqtt.h"
#include "config.h"

static const char *TAG = "app_epd";

/* Max HTTP body for display_url decode path (JPEG/PNG). Raw 960000 also fits. */
#define IMG_URL_MAX_BYTES (768 * 1024)

#define CHUNK_SIZE 8192

static char s_url_err[48];

const char *epd_url_last_error(void)
{
    return s_url_err;
}

static void url_fail(const char *why)
{
    strlcpy(s_url_err, why ? why : "unknown", sizeof(s_url_err));
    ESP_LOGE(TAG, "url frame fail: %s", s_url_err);
}

static unsigned char *s_chunk; /* DMA-capable staging (also used per-line) */

/* ---- async render queue (HTTP/MQTT callers must not block on a refresh) ---- */

typedef struct {
    int kind; /* 0 text, 1 test pattern, 2 canvas, 3 refresh, 4 fill color, 5 frame url */
    espd_render_t render;
    canvas_t canvas;
    unsigned char fillColor;
    char url[512];
    char bootTitle[40];
} render_msg_t;

static QueueHandle_t s_renderQueue;

/* Single panel lock: every path into the panel (render queue jobs, MQTT
 * commands, HTTP frame upload, main's boot/wake renders) serializes here —
 * the SPI bus and the shared DMA chunk must never have two writers. */
static SemaphoreHandle_t s_panelLock;
static bool s_panelOk;
static bool s_userContent; /* true after customer push; SoftAP close must not wipe */

static void epd_lock(void)
{
    if (s_panelLock) {
        xSemaphoreTake(s_panelLock, portMAX_DELAY);
    }
}

static void epd_unlock(void)
{
    if (s_panelLock) {
        xSemaphoreGive(s_panelLock);
    }
}

static void stream_begin(unsigned char csx);
static void stream_end(unsigned char csx);

/* Native (nx,ny) -> logical landscape (lx,ly), inverse of 90° CCW (left). */
static inline void native_to_logic(int nx, int ny, int *lx, int *ly)
{
    *lx = ny;
    *ly = EPD_NATIVE_W - 1 - nx;
}

static void stream_render_unlocked(unsigned char (*pixel_fn)(int x, int y, void *ctx), void *ctx)
{
    /* CS0 = left nx[0..599], CS1 = right nx[600..1199]; each 1600 x 300B. */
    for (int csx = 0; csx < 2; csx++) {
        stream_begin(csx);
        for (int ny = 0; ny < EPD_NATIVE_H; ny++) {
            unsigned char *row = s_chunk;
            int nx_base = csx * EPD_CS_WIDTH_PX;
            for (int b = 0; b < EPD_CS_LINE_BYTES; b++) {
                int nx = nx_base + b * 2;
                int lx0, ly0, lx1, ly1;
                native_to_logic(nx, ny, &lx0, &ly0);
                native_to_logic(nx + 1, ny, &lx1, &ly1);
                unsigned char hi = pixel_fn(lx0, ly0, ctx);
                unsigned char lo = pixel_fn(lx1, ly1, ctx);
                row[b] = (hi & 0x0F) << 4 | (lo & 0x0F);
            }
            writeEpdData(row, EPD_CS_LINE_BYTES);
        }
        stream_end(csx);
    }
    epdDisplay();
}

void epd_stream_render(unsigned char (*pixel_fn)(int x, int y, void *ctx), void *ctx)
{
    epd_lock();
    stream_render_unlocked(pixel_fn, ctx);
    epd_unlock();
}

static void render_task(void *arg)
{
    /* Queue carries pointers — canvas_t grew with CANVAS_MAX_ITEMS; never
     * copy a full render_msg_t onto this task's stack. */
    render_msg_t *msg = NULL;
    while (xQueueReceive(s_renderQueue, &msg, portMAX_DELAY)) {
        if (!msg) {
            continue;
        }
        switch (msg->kind) {
        case 1: {
            const char *lines[] = { "EPAPER OK", "six color test" };
            epd_text_screen(lines, 2, 0x00, 0x11, 4);
            break;
        }
        case 2:
            canvas_render(&msg->canvas);
            break;
        case 3:
            epd_lock();
            epdDisplay();
            epd_unlock();
            break;
        case 4:
            epd_fill_refresh(msg->fillColor);
            break;
        case 5:
            if (epd_url_frame(msg->url) != 0) {
                /* Do not blame the URL for decode/OOM — show the real reason
                 * plus how to get SoftAP-quality frames on this 2MB device. */
                const char *why = epd_url_last_error();
                if (!why || !why[0]) {
                    why = "unknown error";
                }
                char why_line[48];
                snprintf(why_line, sizeof(why_line), "Reason: %s", why);
                const char *err[] = {
                    "Image display failed",
                    why_line,
                    "",
                    "For best results use:",
                    "1. SoftAP web Image upload",
                    "2. Preconvert 6-color raw",
                    "   (960000 bytes 4bpp)",
                    "3. JPEG/PNG <300KB, no WebP",
                };
                epd_text_screen(err, 8, 0x00, 0x11, 2);
                mqtt_publish_cmd_ack("display_url", "error", why);
            } else {
                mqtt_publish_cmd_ack("display_url", "ok", "displayed");
            }
            break;
        case 7: {
            /* embedded test frame: stream flash -> panel */
            if (epd_upload_start() == 0) {
                for (unsigned long off = 0; off < 960000;) {
                    int n = (960000 - off) > CHUNK_SIZE ? CHUNK_SIZE : (int)(960000 - off);
                    memcpy(s_chunk, FRAME_TEST + off, n);
                    epd_upload_write(s_chunk, n);
                    off += n;
                }
                epd_upload_finish();
            }
            break;
        }
        case 8:
            ESP_LOGI(TAG, "render: default splash");
            splash_show_default();
            break;
        case 6: {
            const char *linePtrs[EPD_TEXT_MAX_LINES] = { 0 };
            for (int i = 0; i < msg->render.count && i < EPD_TEXT_MAX_LINES; i++) {
                linePtrs[i] = msg->render.lines[i];
            }
            epd_boot_screen(msg->bootTitle, linePtrs, msg->render.count);
            break;
        }
        default: {
            const char *linePtrs[EPD_TEXT_MAX_LINES] = { 0 };
            for (int i = 0; i < msg->render.count && i < EPD_TEXT_MAX_LINES; i++) {
                linePtrs[i] = msg->render.lines[i];
            }
            epd_text_screen(linePtrs, msg->render.count, msg->render.fg, msg->render.bg,
                            msg->render.scale);
            break;
        }
        }
        free(msg);
        msg = NULL;
    }
}

/* Build queue items on the heap — render_msg_t embeds canvas_t (~12KB with
 * 48 items) and must never be a stack local on mqtt_cmd / HTTP paths.
 * Ownership transfers to render_task (pointer queue). */
static render_msg_t *alloc_render_msg(int kind)
{
    render_msg_t *msg = calloc(1, sizeof(*msg));
    if (msg) {
        msg->kind = kind;
    }
    return msg;
}

static int queue_render_owned(render_msg_t *msg, TickType_t wait)
{
    if (!msg) {
        return -1;
    }
    if (!s_renderQueue) {
        free(msg);
        return -1;
    }
    BaseType_t ok = xQueueSend(s_renderQueue, &msg, wait);
    if (ok != pdTRUE) {
        ESP_LOGW(TAG, "render busy");
        free(msg);
        return -1;
    }
    return 0;
}

void app_epd_note_user_content(void)
{
    s_userContent = true;
}

void app_epd_note_system_frame(void)
{
    s_userContent = false;
}

bool app_epd_has_user_content(void)
{
    return s_userContent;
}

int app_epd_render_async(const espd_render_t *render)
{
    render_msg_t *msg = alloc_render_msg(0);
    if (!msg) {
        return -1;
    }
    msg->render = *render;
    if (msg->render.count > EPD_TEXT_MAX_LINES) {
        msg->render.count = EPD_TEXT_MAX_LINES;
    }
    for (int i = 0; i < msg->render.count; i++) {
        strlcpy(msg->render.lines[i], render->lines[i], sizeof(msg->render.lines[i]));
    }
    if (msg->render.scale < 1) {
        msg->render.scale = 1;
    }
    int rc = queue_render_owned(msg, 0);
    if (rc == 0) {
        app_epd_note_user_content();
    }
    return rc;
}

int app_epd_test_pattern_async(void)
{
    render_msg_t *msg = alloc_render_msg(1);
    int rc = queue_render_owned(msg, 0);
    if (rc == 0) {
        app_epd_note_user_content();
    }
    return rc;
}

int app_epd_canvas_async(const canvas_t *cv)
{
    render_msg_t *msg = alloc_render_msg(2);
    if (!msg) {
        return -1;
    }
    msg->canvas = *cv; /* deep copy: QR bitmaps ride inside the struct */
    /* Allow a short wait so a finishing splash does not reject the push. */
    int rc = queue_render_owned(msg, pdMS_TO_TICKS(3000));
    if (rc == 0) {
        /* SoftAP setup also uses canvas — caller clears via note_system_frame. */
        app_epd_note_user_content();
    }
    return rc;
}

int app_epd_refresh_async(void)
{
    render_msg_t *msg = alloc_render_msg(3);
    int rc = queue_render_owned(msg, 0);
    if (rc == 0) {
        app_epd_note_user_content();
    }
    return rc;
}

int app_epd_boot_screen_async(const char *title, const char *const *lines, int count)
{
    render_msg_t *msg = alloc_render_msg(6);
    if (!msg) {
        return -1;
    }
    if (strlen(title) >= sizeof(msg->bootTitle)) {
        free(msg);
        return -1;
    }
    strlcpy(msg->bootTitle, title, sizeof(msg->bootTitle));
    if (count > EPD_TEXT_MAX_LINES) {
        count = EPD_TEXT_MAX_LINES;
    }
    for (int i = 0; i < count; i++) {
        strlcpy(msg->render.lines[i], lines[i], sizeof(msg->render.lines[i]));
    }
    msg->render.count = count;
    return queue_render_owned(msg, 0);
}

int app_epd_setup_screen_async(const char *ssid)
{
    /* canvas_t is large — allocate on heap (caller stacks are small). */
    canvas_t *cv = malloc(sizeof(*cv));
    if (!cv) {
        return -1;
    }
    int rc = canvas_fill_setup_screen(cv, ssid);
    if (rc != 0) {
        free(cv);
        return rc;
    }
    rc = app_epd_canvas_async(cv);
    free(cv);
    if (rc == 0) {
        app_epd_note_system_frame(); /* not customer art */
    }
    return rc;
}

int app_epd_test_frame_async(void)
{
    render_msg_t *msg = alloc_render_msg(7);
    int rc = queue_render_owned(msg, 0);
    if (rc == 0) {
        app_epd_note_user_content();
    }
    return rc;
}

void epd_logo_screen(void)
{
    splash_show_default();
    app_epd_note_system_frame();
}

int app_epd_splash_async(void)
{
    render_msg_t *msg = alloc_render_msg(8);
    int rc = queue_render_owned(msg, 0);
    if (rc == 0) {
        app_epd_note_system_frame();
    }
    return rc;
}

int app_epd_fill_async(unsigned char color_code)
{
    render_msg_t *msg = alloc_render_msg(4);
    if (!msg) {
        return -1;
    }
    msg->fillColor = color_code;
    int rc = queue_render_owned(msg, 0);
    if (rc == 0) {
        app_epd_note_user_content();
    }
    return rc;
}

int app_epd_url_async(const char *url)
{
    render_msg_t *msg = alloc_render_msg(5);
    if (!msg) {
        return -1;
    }
    if (strlen(url) >= sizeof(msg->url)) {
        free(msg);
        return -1;
    }
    strlcpy(msg->url, url, sizeof(msg->url));
    int rc = queue_render_owned(msg, 0);
    if (rc == 0) {
        app_epd_note_user_content();
    }
    return rc;
}

/* ---- raw frame upload streaming (http.c drives it chunk by chunk) ---- */

static unsigned long s_uploadCount;
static int s_uploadCsx = -1;

unsigned char *app_epd_dma_buf(void)
{
    return s_chunk;
}

int app_epd_dma_buf_size(void)
{
    return CHUNK_SIZE;
}

int epd_upload_start(void)
{
    /* Full refresh holds the lock 20–40s; allow waiting out one in-flight job. */
    if (!s_panelLock || xSemaphoreTake(s_panelLock, pdMS_TO_TICKS(60000)) != pdTRUE) {
        ESP_LOGW(TAG, "panel busy, upload rejected");
        return -1;
    }
    s_uploadCount = 0;
    s_uploadCsx = 0;
    stream_begin(0);
    return 0;
}

int epd_upload_write(const unsigned char *dmaChunk, int len)
{
    if (s_uploadCsx < 0 || !dmaChunk || len <= 0) {
        return -1;
    }
    /* CHUNK_SIZE (8192) does not divide EPD_HALF_BYTES (480000). A single
     * recv can straddle the CS0/CS1 boundary — must split or CS1 never starts
     * and the panel only shows one half. */
    int off = 0;
    while (off < len) {
        unsigned long half_end = (unsigned long)(s_uploadCsx + 1) * EPD_HALF_BYTES;
        unsigned long room = half_end - s_uploadCount;
        if (room == 0) {
            return -1;
        }
        int n = len - off;
        if ((unsigned long)n > room) {
            n = (int)room;
        }
        writeEpdData((unsigned char *)dmaChunk + off, n);
        s_uploadCount += n;
        off += n;
        if (s_uploadCsx == 0 && s_uploadCount == EPD_HALF_BYTES) {
            stream_end(0);
            s_uploadCsx = 1;
            stream_begin(1);
        }
    }
    return 0;
}

int epd_upload_finish(void)
{
    if (s_uploadCsx < 0) {
        return -1;
    }
    stream_end(s_uploadCsx);
    s_uploadCsx = -1;
    if (s_uploadCount != EPD_HALF_BYTES * 2) {
        ESP_LOGW(TAG, "upload size mismatch: %lu", s_uploadCount);
    }
    /* Refresh while still holding the panel lock so nothing else can
     * interleave DTM/PON with this frame (avoids dirty corners). */
    epdDisplay();
    epd_unlock();
    app_epd_note_user_content();
    return 0;
}

void epd_upload_abort(void)
{
    if (s_uploadCsx >= 0) {
        stream_end(s_uploadCsx);
        s_uploadCsx = -1;
    }
    s_uploadCount = 0;
    epd_unlock();
}

int app_epd_init(void)
{
    if (!s_panelLock) {
        s_panelLock = xSemaphoreCreateMutex();
    }
    if (!s_chunk) {
        s_chunk = heap_caps_malloc(CHUNK_SIZE, MALLOC_CAP_DMA);
        if (!s_chunk) {
            ESP_LOGE(TAG, "no DMA memory");
            return -1;
        }
    }
    if (!s_renderQueue) {
        s_renderQueue = xQueueCreate(2, sizeof(render_msg_t *));
        /* display_url does HTTPS+decode on this task — TLS needs a large stack. */
        xTaskCreate(render_task, "epd_render", 32768, NULL, 4, NULL);
    }

    /* SPI_CTL early so SGM3699 stays on EPD while CS/RST come up. */
    spiCtlSelectEpd(1);

    /* VE071A bring-up: AW9523B panel power, then SPI to the EPD side. */
    if (board_ioext_init() != ESP_OK) {
        ESP_LOGE(TAG, "ioext init failed");
        return -1;
    }
    if (board_ioext_epad_power(true) != ESP_OK) {
        ESP_LOGE(TAG, "EPAD power on failed");
        return -1;
    }

    initialGpioDefault(); /* float bus until VDD / BUSY rise */
    vTaskDelay(pdMS_TO_TICKS(100)); /* EPAD_VDD settle (VE071A epd_7in09_e6) */
    for (int i = 0; i < 5; i++) {
        ESP_LOGI(TAG, "busy sample[%d/5] = %d", i, getGpioLevel(EPD_BUSY));
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    checkBusyHigh();
    initialGpio(); /* CS0/CS1/RST driven high */
    if (initialSpi() != DONE) {
        ESP_LOGE(TAG, "initialSpi failed");
        return -1;
    }
    epdHardwareReset();
    setPinCsAll(GPIO_HIGH);
    initEPD();
    spiCtlSelectEpd(1); /* ensure mux still on EPD after init traffic */
    s_panelOk = (checkDriverICStatus() == DONE);
    if (!s_panelOk) {
        /* Keep streaming attempts (BUSY rose + SPI init), but report unhealthy. */
        ESP_LOGW(TAG, "driver IC status not ready — draws allowed, panel_ok=false");
    }
    return 0;
}

bool app_epd_panel_ok(void)
{
    return s_panelOk;
}

static void stream_begin(unsigned char csx)
{
    if (csx == 0) {
        executeTscCommand();
    }
    setPinCs(csx, GPIO_LOW);
    writeEpdCommand(DTM);
}

static void stream_end(unsigned char csx)
{
    setPinCs(csx, GPIO_HIGH);
}

void epd_fill_refresh(unsigned char color_code)
{
    epd_lock();
    memset(s_chunk, color_code, CHUNK_SIZE);
    for (int csx = 0; csx < 2; csx++) {
        stream_begin(csx);
        for (unsigned long sent = 0; sent < EPD_HALF_BYTES; sent += CHUNK_SIZE) {
            unsigned long n = EPD_HALF_BYTES - sent;
            if (n > CHUNK_SIZE) {
                n = CHUNK_SIZE;
            }
            writeEpdData(s_chunk, (unsigned int)n);
        }
        stream_end(csx);
    }
    epdDisplay();
    epd_unlock();
}

/* ---- text rendering (virtual framebuffer, evaluated per pixel) ---- */

typedef struct {
    const char *title;             /* big line near the top (optional)  */
    int title_scale, title_top;
    const char *const *lines;      /* body lines                        */
    int line_count;
    unsigned char fg, bg;
    int scale;
    int line_h;                    /* FONT_H * scale + gap              */
    int total_h;
    int top;                       /* first body pixel row (logical y)  */
} text_layout_t;

static unsigned char glyph_bit(char c, int row, int col)
{
    if (c < FONT_FIRST || c > FONT_LAST) {
        c = '?';
    }
    const unsigned char *g = font8x16[c - FONT_FIRST];
    return (g[row] >> (FONT_W - 1 - col)) & 1;
}

static unsigned char centered_glyph_pixel(const char *line, int scale,
                                          unsigned char fg, unsigned char bg,
                                          int x, int y)
{
    int line_px = (int)strlen(line) * FONT_W * scale;
    int x0 = (EPD_PANEL_W - line_px) / 2;
    if (x < x0 || x >= x0 + line_px) {
        return bg;
    }
    int lx = x - x0;
    int ci = lx / (FONT_W * scale);
    int gx = (lx % (FONT_W * scale)) / scale;
    int gy = y / scale;
    if (ci >= (int)strlen(line)) {
        return bg;
    }
    return glyph_bit(line[ci], gy, gx) ? fg : bg;
}

static unsigned char text_pixel(const text_layout_t *tl, int x, int y)
{
    if (tl->title && y >= tl->title_top &&
        y < tl->title_top + FONT_H * tl->title_scale) {
        return centered_glyph_pixel(tl->title, tl->title_scale, tl->fg, tl->bg,
                                    x, y - tl->title_top);
    }
    if (y < tl->top || y >= tl->top + tl->total_h) {
        return tl->bg;
    }
    int ty = y - tl->top;
    int li = ty / tl->line_h;
    int gy = ty % tl->line_h;
    if (li >= tl->line_count || gy >= FONT_H * tl->scale || !tl->lines[li]) {
        return tl->bg;
    }
    return centered_glyph_pixel(tl->lines[li], tl->scale, tl->fg, tl->bg, x, gy);
}

static unsigned char layout_pixel_fn(int x, int y, void *ctx)
{
    return text_pixel((const text_layout_t *)ctx, x, y);
}

static void render_layout_locked(const text_layout_t *tl)
{
    stream_render_unlocked(layout_pixel_fn, (void *)tl);
}

void epd_text_screen(const char *const *lines, int line_count,
                     unsigned char fg_code, unsigned char bg_code, int scale)
{
    /* Route through canvas so UTF-8 CJK (GB2312) works — the old ASCII-only
     * glyph path showed '?' / garbage for remote Chinese render_text. */
    canvas_t *cv = calloc(1, sizeof(*cv));
    if (!cv) {
        ESP_LOGE(TAG, "text screen OOM");
        return;
    }
    if (canvas_fill_text_screen(cv, lines, line_count, fg_code, bg_code, scale) != 0) {
        free(cv);
        return;
    }
    canvas_render(cv);
    free(cv);
}

void epd_boot_screen(const char *title, const char *const *lines, int line_count)
{
    text_layout_t tl = {
        .title = title, .title_scale = 4, .title_top = 120,
        .lines = lines, .line_count = line_count,
        .fg = BLACK, .bg = WHITE, .scale = 2,
    };
    tl.line_h = FONT_H * 2 + 4;
    tl.total_h = tl.line_h * line_count;
    tl.top = 320;
    if (tl.top + tl.total_h > EPD_PANEL_H - 40) {
        tl.top = EPD_PANEL_H - 40 - tl.total_h;
    }
    epd_lock();
    render_layout_locked(&tl);
    epd_unlock();
}

/* ---- HTTP frame streaming ---- */

static unsigned char logical_fb_pixel(int lx, int ly, void *ctx)
{
    const uint8_t *fb = (const uint8_t *)ctx;
    if (lx < 0 || ly < 0 || lx >= IMG_PANEL_W || ly >= IMG_PANEL_H) {
        return 0x1;
    }
    size_t off = (size_t)ly * (IMG_PANEL_W / 2) + (size_t)(lx / 2);
    uint8_t b = fb[off];
    return (lx & 1) ? (b & 0x0F) : (b >> 4);
}

static int stream_raw_native(const uint8_t *raw, size_t len)
{
    if (len != (size_t)EPD_HALF_BYTES * 2) {
        return -1;
    }
    size_t off = 0;
    for (int csx = 0; csx < 2; csx++) {
        stream_begin(csx);
        for (unsigned long sent = 0; sent < EPD_HALF_BYTES;) {
            int n = (int)((EPD_HALF_BYTES - sent) > CHUNK_SIZE ? CHUNK_SIZE
                                                               : (EPD_HALF_BYTES - sent));
            memcpy(s_chunk, raw + off, (size_t)n);
            writeEpdData(s_chunk, n);
            off += (size_t)n;
            sent += n;
        }
        stream_end(csx);
    }
    epdDisplay();
    return 0;
}

/* Sparse stage text on panel (QUALITY URL path only — each call is a full refresh). */
static void url_stage(int show, const char *stage)
{
    if (!show || !stage) {
        return;
    }
    const char *lines[] = { "URL image", stage };
    epd_text_screen(lines, 2, 0x00, 0x11, 3);
}

int epd_url_frame(const char *url)
{
    s_url_err[0] = '\0';
    ESP_LOGI(TAG, "url frame start: %.96s%s", url ? url : "",
             (url && strlen(url) > 96) ? "…" : "");

    powerAttr_t pwr;
    cfg_get_power_attr(&pwr);
    const int quality = (pwr.imageQuality != IMG_QUALITY_SPEED);
    const img_proc_mode_t proc = quality ? IMG_PROC_QUALITY : IMG_PROC_SPEED;
    ESP_LOGI(TAG, "url image mode=%s", img_quality_str(pwr.imageQuality));

    /* Do not stage before download: raw path needs no progress page, and each
     * stage is a full-panel refresh (tens of seconds). JPEG/PNG stages below. */

    const size_t raw_need = (size_t)EPD_HALF_BYTES * 2;
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 60000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .max_redirection_count = 5,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        url_fail("http init fail");
        return -1;
    }

    uint8_t *body = NULL;
    uint8_t *fb = NULL;
    int ret = -1;

    if (esp_http_client_open(client, 0) != ESP_OK) {
        url_fail("http open/TLS fail");
        goto out;
    }
    long clen = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status > 0 && (status < 200 || status >= 300)) {
        char buf[40];
        snprintf(buf, sizeof(buf), "HTTP status %d", status);
        url_fail(buf);
        goto out_close;
    }

    size_t cap = IMG_URL_MAX_BYTES;
    if (clen > 0) {
        if ((size_t)clen > IMG_URL_MAX_BYTES && (size_t)clen != raw_need) {
            url_fail("file >768KB limit");
            goto out_close;
        }
        if ((size_t)clen == raw_need) {
            cap = raw_need;
        } else {
            cap = (size_t)clen;
        }
    }

    body = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!body) {
        body = malloc(cap);
    }
    if (!body) {
        url_fail("download OOM");
        goto out_close;
    }

    size_t total = 0;
    while (total < cap) {
        int want = (int)((cap - total) > CHUNK_SIZE ? CHUNK_SIZE : (cap - total));
        int got = esp_http_client_read(client, (char *)body + total, want);
        if (got < 0) {
            url_fail("http read error");
            goto out_close;
        }
        if (got == 0) {
            break;
        }
        total += (size_t)got;
    }
    ESP_LOGI(TAG, "downloaded %u bytes (clen=%ld)", (unsigned)total, clen);
    if (total == 0) {
        url_fail("empty download");
        goto out_close;
    }

    /* Drop TLS/HTTP buffers before decode+dither — they eat hundreds of KB
     * of PSRAM and were the main cause of dither OOM on 2MB C61. */
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    client = NULL;

    img_fmt_t fmt = img_sniff_format(body, total);
    if (fmt == IMG_FMT_RAW4BPP || (total == raw_need && fmt != IMG_FMT_JPEG && fmt != IMG_FMT_PNG)) {
        epd_lock();
        ret = stream_raw_native(body, total);
        epd_unlock();
        if (ret == 0) {
            ESP_LOGI(TAG, "raw 4bpp frame OK");
        } else {
            url_fail("raw stream fail");
        }
    } else if (fmt == IMG_FMT_JPEG || fmt == IMG_FMT_PNG) {
        url_stage(quality, "Decoding...");
        ESP_LOGI(TAG, "decode %s... (free heap %u spiram %u)",
                 fmt == IMG_FMT_JPEG ? "JPEG" : "PNG",
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        uint8_t *rgb = NULL;
        int rw = 0, rh = 0;
        if (img_decode_to_rgb(body, total, &rgb, &rw, &rh, proc) != 0) {
            url_fail(fmt == IMG_FMT_JPEG ? "JPEG decode fail" : "PNG decode fail");
        } else {
            free(body);
            body = NULL;
            /* Dither is the long step — second (and last) progress refresh. */
            url_stage(quality, "Dithering...");
            ESP_LOGI(TAG, "rgb %dx%d — dither (free %u / spiram %u)", rw, rh,
                     (unsigned)esp_get_free_heap_size(),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
            if (img_rgb_dither_alloc_fb(rgb, rw, rh, &fb, proc) != 0) {
                url_fail("dither OOM/fail");
            } else {
                ESP_LOGI(TAG, "decode OK — streaming to panel");
                epd_lock();
                stream_render_unlocked(logical_fb_pixel, fb);
                epd_unlock();
                ret = 0;
                ESP_LOGI(TAG, "decoded image frame OK (%s)",
                         fmt == IMG_FMT_JPEG ? "JPEG" : "PNG");
            }
        }
    } else if (fmt == IMG_FMT_WEBP) {
        url_fail("WebP: convert on cloud");
    } else {
        url_fail("not JPEG/PNG/raw");
    }

    goto out;

out_close:
    if (client) {
        esp_http_client_close(client);
    }
out:
    if (client) {
        esp_http_client_cleanup(client);
    }
    free(body);
    img_fb_release(fb);
    return ret;
}
