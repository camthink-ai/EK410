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
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_timer.h"

#include "app_epd.h"
#include "evtlog.h"
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
#include "wifi.h"
#include "config.h"
#include "last_frame.h"

static const char *TAG = "app_epd";

/* Pause MQTT TLS before a second HTTPS client — C61 cannot hold both.
 * Also disable Wi‑Fi modem sleep so large CDN downloads do not stall. */
static void url_tls_prepare(void)
{
    wifi_throughput_boost(true);
    /* Preview b64 (~160KB) + HTTPS body fight for the same 2MB SPIRAM. */
    last_frame_preview_drop();
    if (mqtt_stop_wait(4000) != 0) {
        ESP_LOGW(TAG, "url TLS prepare: mqtt still busy — proceed carefully");
    }
    vTaskDelay(pdMS_TO_TICKS(80));
}

static void url_tls_release(void)
{
    wifi_throughput_boost(false);
    /* Large HTTPS can leave httpd down and MQTT stopped. Restore LAN Web
     * first, then MQTT — same policy as SoftAP close. */
    if (!wifi_sta_is_connected() || wifi_softap_is_up() || wifi_ble_prov_is_up()) {
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(800));
    wifi_network_services_restore(true);
    if (!mqtt_is_started()) {
        for (int i = 0; i < 7; i++) {
            vTaskDelay(pdMS_TO_TICKS(1500));
            mqtt_start();
            if (mqtt_is_started()) {
                return;
            }
            ESP_LOGW(TAG, "mqtt restart attempt %d/7 — waiting for heap", i + 1);
        }
        ESP_LOGE(TAG, "mqtt restart failed after URL job — timer will keep retrying");
    }
}

static int lf_to_upload_writer(const uint8_t *data, size_t len, void *ctx)
{
    (void)ctx;
    return epd_upload_write(data, (int)len) == 0 ? 0 : -1;
}

/** Replay flash lastfrm onto the panel (no sticky error text). */
static int panel_replay_last_frame(void)
{
    last_frame_meta_t m;
    if (last_frame_get_meta(&m) != 0 || !m.present) {
        return -1;
    }
    /* Solid clear/fill must not replace real art on boot (looks like a bug). */
    if (strcmp(m.source, "fill") == 0) {
        ESP_LOGW(TAG, "skip lastfrm src=fill — prefer splash");
        return -1;
    }
    if (epd_upload_start() != 0) {
        return -1;
    }
    last_frame_capture_abort(); /* keep existing flash copy */
    if (last_frame_stream(lf_to_upload_writer, NULL) != 0) {
        epd_upload_abort();
        return -1;
    }
    return epd_upload_finish();
}

int app_epd_boot_paint_sync(void)
{
    ESP_LOGI(TAG, "boot paint sync (before Wi-Fi)");
    if (panel_replay_last_frame() == 0) {
        app_epd_note_user_content();
        return 0;
    }
    splash_show_default();
    return 0;
}

/* Max HTTP body for display_url decode path (JPEG/PNG). Raw 960000 also fits.
 * Soft cap keeps PSRAM headroom (body + QUALITY RGB ~720KB). Hard reject
 * before buffering so a huge URL cannot hang/OOM the device. */
#define IMG_URL_MAX_BYTES (1280 * 1024)
/* Deadlines cover network + decode only. Final panel DRF has its own BUSY
 * timeout (~40s). Modem-sleep + large HTTPS used to stall past a short net
 * window (Module.png ~820KB → "URL net timeout" after TLS). */
#define IMG_URL_NET_DEADLINE_MS    120000
#define IMG_URL_DECODE_DEADLINE_MS 180000
#define IMG_URL_HTTP_TIMEOUT_MS    60000
#define IMG_URL_FORCE_SPEED_BYTES  (700 * 1024)

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
    evtlog_event("EPD", "display_url fail: %s", s_url_err);
}

static unsigned char *s_chunk; /* DMA-capable staging (also used per-line) */

/* ---- async render queue (HTTP/MQTT callers must not block on a refresh) ---- */

typedef struct {
    int kind; /* 0 text … 8 splash, 9 SoftAP proxyImage, 10 lastfrm replay */
    bool persist_frame; /* save lastfrm after this job (user content only) */
    char mqtt_ack_cmd[24]; /* non-empty → completion cmd_ack after job */
    espd_render_t render;
    canvas_t canvas;
    unsigned char fillColor;
    char url[512];
    char bootTitle[40];
    epd_proxy_job_t *proxy;       /* kind 9 */
    SemaphoreHandle_t proxy_done; /* kind 9 */
} render_msg_t;

/* Sticky: next queue_render_owned copies into msg->mqtt_ack_cmd (MQTT path). */
static char s_mqtt_ack_next[24];

static QueueHandle_t s_renderQueue;

/* Single panel lock: every path into the panel (render queue jobs, MQTT
 * commands, HTTP frame upload, main's boot/wake renders) serializes here —
 * the SPI bus and the shared DMA chunk must never have two writers. */
static SemaphoreHandle_t s_panelLock;
static bool s_panelOk;
/* Set when BUSY times out / upload aborts — next paint still draws content
 * directly (no intermediate white wipe; that was blanking the panel). */
static bool s_panelDirty = true;
static bool s_userContent; /* true after customer push; SoftAP close must not wipe */
/* Sleep gate: refuse new queue/upload; sleep_prepare HOLDs panel lock. */
static bool s_acceptJobs = true;
static bool s_sleepHoldsLock;
static volatile bool s_displayBusy;
static bool s_lastDisplayOk = true;

#define EPD_COLOR_WHITE 0x11

static void stream_begin(unsigned char csx);
static void stream_end(unsigned char csx);
static void epd_proxy_download(epd_proxy_job_t *job);

static int epd_commit_display(void)
{
    if (s_panelDirty) {
        ESP_LOGI(TAG, "panel dirty — content full refresh (no pre-wipe)");
    }
    int ok = epdDisplay();
    if (!ok) {
        s_panelDirty = true;
        s_lastDisplayOk = false;
        ESP_LOGW(TAG, "display incomplete — panel marked dirty");
        evtlog_event("EPD", "display incomplete (BUSY timeout)");
    } else {
        s_panelDirty = false;
        s_lastDisplayOk = true;
    }
    return ok;
}

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

/* Native (nx,ny) -> logical landscape (lx,ly), inverse of 90° CCW (left). */
static inline void native_to_logic(int nx, int ny, int *lx, int *ly)
{
    *lx = ny;
    *ly = EPD_NATIVE_W - 1 - nx;
}

static char s_stream_cap_src[LAST_FRAME_SOURCE_MAX];

void app_epd_mark_frame_capture(const char *source)
{
    if (source && source[0]) {
        strlcpy(s_stream_cap_src, source, sizeof(s_stream_cap_src));
    } else {
        s_stream_cap_src[0] = '\0';
    }
}

static bool stream_render_unlocked(unsigned char (*pixel_fn)(int x, int y, void *ctx), void *ctx)
{
    const bool cap = last_frame_capturing();
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
            if (cap) {
                last_frame_capture_write(row, (size_t)EPD_CS_LINE_BYTES);
            }
            /* Cover sampling is CPU-heavy — yield so IDLE can pet TWDT. */
            if ((ny & 31) == 0) {
                vTaskDelay(1);
            }
        }
        stream_end(csx);
    }
    return epd_commit_display() != 0;
}

void epd_stream_render(unsigned char (*pixel_fn)(int x, int y, void *ctx), void *ctx)
{
    epd_lock();
    bool do_cap = s_stream_cap_src[0] != '\0';
    if (do_cap) {
        if (last_frame_capture_begin(s_stream_cap_src) != 0) {
            do_cap = false;
        }
        s_stream_cap_src[0] = '\0';
    }
    bool ok = stream_render_unlocked(pixel_fn, ctx);
    if (do_cap) {
        if (ok) {
            last_frame_capture_commit();
        } else {
            last_frame_capture_abort();
        }
    }
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
        s_displayBusy = true;
        switch (msg->kind) {
        case 1: {
            const char *lines[] = { "EPAPER OK", "six color test" };
            if (msg->persist_frame) {
                app_epd_mark_frame_capture("test");
            }
            epd_text_screen(lines, 2, 0x00, 0x11, 4);
            break;
        }
        case 2:
            if (msg->persist_frame) {
                app_epd_mark_frame_capture("canvas");
            }
            canvas_render(&msg->canvas);
            break;
        case 3:
            /* Bare refresh of previous LUT — do not wipe to white first. */
            epd_lock();
            (void)epd_commit_display();
            epd_unlock();
            break;
        case 4:
            if (msg->persist_frame) {
                app_epd_mark_frame_capture("fill");
            }
            epd_fill_refresh(msg->fillColor);
            break;
        case 5:
            if (epd_url_frame(msg->url) != 0) {
                const char *why = epd_url_last_error();
                if (!why || !why[0]) {
                    why = "unknown error";
                }
                /* E-ink keeps the last paint forever — prefer replaying the
                 * saved frame over a sticky "http init fail" wall of text. */
                if (panel_replay_last_frame() != 0) {
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
                        "3. JPEG/PNG under size limit, no WebP",
                    };
                    /* Avoid painting a full error wall for size/timeout — keep art. */
                    if (strstr(why, "limit") || strstr(why, "timeout") ||
                        strstr(why, "OOM")) {
                        ESP_LOGW(TAG, "url fail (%s) — keep last frame (no error wall)", why);
                    } else {
                        epd_text_screen(err, 8, 0x00, 0x11, 2);
                    }
                } else {
                    ESP_LOGW(TAG, "url fail (%s) — restored last frame", why);
                }
                s_lastDisplayOk = false;
                mqtt_publish_cmd_ack("display_url", "error", why);
            } else {
                s_lastDisplayOk = true;
                mqtt_publish_cmd_ack("display_url", "ok", "displayed");
            }
            break;
        case 7: {
            /* embedded test frame: stream flash -> panel */
            if (epd_upload_start() == 0) {
                last_frame_capture_begin("testframe");
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
        case 10:
            ESP_LOGI(TAG, "render: restore last frame");
            if (panel_replay_last_frame() != 0) {
                ESP_LOGW(TAG, "last frame missing — CamThink splash");
                splash_show_default();
            } else {
                app_epd_note_user_content();
            }
            break;
        case 9:
            if (msg->proxy) {
                epd_proxy_download(msg->proxy);
                if (msg->proxy->orphaned) {
                    free(msg->proxy->data);
                    if (msg->proxy_done) {
                        vSemaphoreDelete(msg->proxy_done);
                    }
                    free(msg->proxy);
                    msg->proxy = NULL;
                    ESP_LOGW(TAG, "proxy fetch orphan cleaned");
                } else if (msg->proxy_done) {
                    xSemaphoreGive(msg->proxy_done);
                }
            } else if (msg->proxy_done) {
                xSemaphoreGive(msg->proxy_done);
            }
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
            if (msg->persist_frame) {
                app_epd_mark_frame_capture("text");
            }
            if (msg->render.text[0]) {
                /* Wrap on this 32KB task — mqtt_cmd must not own wrap buffers. */
                epd_text_paragraph(msg->render.text, msg->render.fg, msg->render.bg,
                                   msg->render.scale);
            } else {
                const char *linePtrs[EPD_TEXT_MAX_LINES] = { 0 };
                for (int i = 0; i < msg->render.count && i < EPD_TEXT_MAX_LINES; i++) {
                    linePtrs[i] = msg->render.lines[i];
                }
                epd_text_screen(linePtrs, msg->render.count, msg->render.fg,
                                msg->render.bg, msg->render.scale);
            }
            break;
        }
        }
        /* MQTT completion ack for clear/render_* (display_url already acked). */
        if (msg->kind != 5 && msg->mqtt_ack_cmd[0]) {
            mqtt_publish_cmd_ack(msg->mqtt_ack_cmd,
                                 s_lastDisplayOk ? "ok" : "error",
                                 s_lastDisplayOk ? "displayed" : "display failed");
        }
        free(msg);
        msg = NULL;
        s_displayBusy = false;
    }
}

bool app_epd_display_busy(void)
{
    UBaseType_t queued = s_renderQueue ? uxQueueMessagesWaiting(s_renderQueue) : 0;
    return s_displayBusy || queued > 0;
}

bool app_epd_last_display_ok(void)
{
    return s_lastDisplayOk;
}

/* Build queue items on the heap — render_msg_t embeds canvas_t (~12KB with
 * 48 items) and must never be a stack local on mqtt_cmd / HTTP paths.
 * Ownership transfers to render_task (pointer queue). */
void app_epd_mqtt_ack_next(const char *cmd)
{
    if (cmd && cmd[0]) {
        strlcpy(s_mqtt_ack_next, cmd, sizeof(s_mqtt_ack_next));
    } else {
        s_mqtt_ack_next[0] = '\0';
    }
}

static render_msg_t *alloc_render_msg(int kind)
{
    render_msg_t *msg = calloc(1, sizeof(*msg));
    if (msg) {
        msg->kind = kind;
    }
    return msg;
}

/* 0=queued, -1=queue full, -2=sleep pending, -3=internal */
static int queue_render_owned(render_msg_t *msg, TickType_t wait)
{
    if (!msg) {
        return -3;
    }
    if (s_mqtt_ack_next[0]) {
        strlcpy(msg->mqtt_ack_cmd, s_mqtt_ack_next, sizeof(msg->mqtt_ack_cmd));
        s_mqtt_ack_next[0] = '\0';
    }
    if (!s_acceptJobs) {
        ESP_LOGW(TAG, "render rejected — sleep pending");
        free(msg);
        return -2;
    }
    if (!s_renderQueue) {
        free(msg);
        return -3;
    }
    BaseType_t ok = xQueueSend(s_renderQueue, &msg, wait);
    if (ok != pdTRUE) {
        ESP_LOGW(TAG, "render queue full");
        free(msg);
        return -1;
    }
    return 0;
}

/* Drop queued jobs so a fresh uploadFrame is not overwritten by an older
 * MQTT/canvas/text job that arrived while the panel lock was held. */
static void render_queue_drop_pending(const char *reason)
{
    if (!s_renderQueue) {
        return;
    }
    int n = 0;
    render_msg_t *msg = NULL;
    while (xQueueReceive(s_renderQueue, &msg, 0) == pdTRUE) {
        if (!msg) {
            continue;
        }
        if (msg->kind == 9 && msg->proxy) {
            msg->proxy->orphaned = true;
            if (msg->proxy_done) {
                xSemaphoreGive(msg->proxy_done);
            }
            /* proxy buffer freed by waiter or orphan path — do not free data here */
        } else if (msg->proxy_done) {
            xSemaphoreGive(msg->proxy_done);
        }
        free(msg);
        msg = NULL;
        n++;
    }
    if (n > 0) {
        ESP_LOGW(TAG, "dropped %d pending render job(s) (%s)", n,
                 reason ? reason : "superseded");
    }
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
    if (!msg || !render) {
        free(msg);
        return -1;
    }
    msg->persist_frame = true;
    memset(&msg->render, 0, sizeof(msg->render));
    msg->render.fg = render->fg;
    msg->render.bg = render->bg;
    msg->render.scale = render->scale;
    if (msg->render.scale < 1) {
        msg->render.scale = 1;
    }
    if (msg->render.scale > CANVAS_TEXT_SCALE_MAX) {
        msg->render.scale = CANVAS_TEXT_SCALE_MAX;
    }
    if (render->text[0]) {
        canvas_utf8_strlcpy(msg->render.text, render->text, sizeof(msg->render.text));
    } else {
        if (render->count > EPD_TEXT_MAX_LINES) {
            msg->render.count = EPD_TEXT_MAX_LINES;
        } else {
            msg->render.count = render->count;
        }
        for (int i = 0; i < msg->render.count; i++) {
            canvas_utf8_strlcpy(msg->render.lines[i], render->lines[i],
                                sizeof(msg->render.lines[i]));
        }
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
    if (!msg) {
        return -1;
    }
    msg->persist_frame = true;
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
    msg->persist_frame = true;
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
    if (!msg) {
        return -1;
    }
    msg->persist_frame = true;
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
    /* Queue directly with persist_frame=false — SoftAP QR is not user art. */
    render_msg_t *msg = alloc_render_msg(2);
    if (!msg) {
        free(cv);
        return -1;
    }
    msg->persist_frame = false;
    msg->canvas = *cv;
    free(cv);
    rc = queue_render_owned(msg, pdMS_TO_TICKS(3000));
    if (rc == 0) {
        app_epd_note_system_frame();
    }
    return rc;
}

int app_epd_test_frame_async(void)
{
    render_msg_t *msg = alloc_render_msg(7);
    if (!msg) {
        return -1;
    }
    msg->persist_frame = true;
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

int app_epd_last_frame_async(void)
{
    last_frame_meta_t m;
    if (last_frame_get_meta(&m) != 0 || !m.present) {
        return -1;
    }
    render_msg_t *msg = alloc_render_msg(10);
    return queue_render_owned(msg, 0);
}

int app_epd_fill_async(unsigned char color_code)
{
    render_msg_t *msg = alloc_render_msg(4);
    if (!msg) {
        return -1;
    }
    /* Do not persist solid fills (esp. white clear) as lastfrm — boot would
     * restore a blank panel and look like content was wiped. */
    msg->persist_frame = false;
    msg->fillColor = color_code;
    return queue_render_owned(msg, 0);
}

int app_epd_url_async(const char *url)
{
    render_msg_t *msg = alloc_render_msg(5);
    if (!msg) {
        return -1;
    }
    msg->persist_frame = true;
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

int app_epd_proxy_fetch(epd_proxy_job_t *job, int timeout_ms)
{
    if (!job || !job->url[0]) {
        return -1;
    }
    job->data = NULL;
    job->len = 0;
    job->ctype[0] = '\0';
    job->err[0] = '\0';
    job->orphaned = false;

    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    if (!done) {
        snprintf(job->err, sizeof(job->err), "OOM");
        return -1;
    }
    render_msg_t *msg = alloc_render_msg(9);
    if (!msg) {
        vSemaphoreDelete(done);
        snprintf(job->err, sizeof(job->err), "OOM");
        return -1;
    }
    msg->proxy = job;
    msg->proxy_done = done;
    /* Allow splash / prior URL job to finish — panel refresh is 20–40s. */
    if (queue_render_owned(msg, pdMS_TO_TICKS(60000)) != 0) {
        vSemaphoreDelete(done);
        snprintf(job->err, sizeof(job->err), "render busy");
        return -1;
    }
    int wait = (timeout_ms > 0) ? timeout_ms : 90000;
    if (xSemaphoreTake(done, pdMS_TO_TICKS(wait)) != pdTRUE) {
        ESP_LOGE(TAG, "proxy fetch timeout");
        snprintf(job->err, sizeof(job->err), "proxy timeout");
        job->orphaned = true;
        return -1;
    }
    vSemaphoreDelete(done);
    return job->err[0] ? -1 : 0;
}

#define PROXY_FETCH_MAX (768 * 1024)
#define PROXY_FETCH_CHUNK 4096

static void epd_proxy_download(epd_proxy_job_t *job)
{
    if (!job) {
        return;
    }
    ESP_LOGI(TAG, "proxy fetch %.96s%s", job->url, strlen(job->url) > 96 ? "…" : "");
    url_tls_prepare();
    esp_http_client_config_t cfg = {
        .url = job->url,
        .timeout_ms = 25000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .max_redirection_count = 5,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        snprintf(job->err, sizeof(job->err), "http init fail");
        ESP_LOGE(TAG, "proxy http init fail — free %u int %u largest_int %u",
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        url_tls_release();
        return;
    }
    if (esp_http_client_open(client, 0) != ESP_OK) {
        snprintf(job->err, sizeof(job->err), "http open/TLS fail");
        esp_http_client_cleanup(client);
        return;
    }
    long clen = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status > 0 && (status < 200 || status >= 300)) {
        snprintf(job->err, sizeof(job->err), "HTTP status %d", status);
        goto out_close;
    }
    if (clen > (long)PROXY_FETCH_MAX) {
        snprintf(job->err, sizeof(job->err), "file >768KB limit");
        goto out_close;
    }
    size_t cap = (clen > 0) ? (size_t)clen : PROXY_FETCH_MAX;
    uint8_t *body = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!body) {
        body = malloc(cap);
    }
    if (!body) {
        snprintf(job->err, sizeof(job->err), "download OOM");
        goto out_close;
    }
    size_t total = 0;
    while (total < cap) {
        int want = (int)((cap - total) > PROXY_FETCH_CHUNK ? PROXY_FETCH_CHUNK : (cap - total));
        int got = esp_http_client_read(client, (char *)body + total, want);
        if (got < 0) {
            free(body);
            snprintf(job->err, sizeof(job->err), "http read error");
            goto out_close;
        }
        if (got == 0) {
            break;
        }
        total += (size_t)got;
    }
    if (total == 0) {
        free(body);
        snprintf(job->err, sizeof(job->err), "empty download");
        goto out_close;
    }
    img_fmt_t fmt = img_sniff_format(body, total);
    if (fmt == IMG_FMT_JPEG) {
        snprintf(job->ctype, sizeof(job->ctype), "image/jpeg");
    } else if (fmt == IMG_FMT_PNG) {
        snprintf(job->ctype, sizeof(job->ctype), "image/png");
    } else if (fmt == IMG_FMT_WEBP) {
        snprintf(job->ctype, sizeof(job->ctype), "image/webp");
    } else {
        free(body);
        snprintf(job->err, sizeof(job->err), "not JPEG/PNG/WebP");
        goto out_close;
    }
    job->data = body;
    job->len = total;
    ESP_LOGI(TAG, "proxy fetch ok %u %s", (unsigned)total, job->ctype);

out_close:
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    url_tls_release();
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
    if (!s_acceptJobs) {
        ESP_LOGW(TAG, "upload rejected — sleep pending");
        return -1;
    }
    /* Full refresh holds the lock 20–40s; allow waiting out one in-flight job.
     * GC alone can take ~60s when dirty — wait long enough. */
    if (!s_panelLock || xSemaphoreTake(s_panelLock, pdMS_TO_TICKS(180000)) != pdTRUE) {
        ESP_LOGW(TAG, "panel busy, upload rejected");
        return -1;
    }
    if (!s_acceptJobs) {
        xSemaphoreGive(s_panelLock);
        ESP_LOGW(TAG, "upload rejected — sleep pending after wait");
        return -1;
    }
    /* Anything still queued would run after we unlock and paint over this upload. */
    render_queue_drop_pending("upload start");
    s_uploadCount = 0;
    s_uploadCsx = 0;
    stream_begin(0);
    /* Default source; caller may restart capture with another label. */
    last_frame_capture_begin("upload");
    return 0;
}

int epd_upload_write(const unsigned char *dmaChunk, int len)
{
    if (s_uploadCsx < 0 || !dmaChunk || len <= 0 || !s_chunk) {
        return -1;
    }
    /* CHUNK_SIZE (8192) does not divide EPD_HALF_BYTES (480000). A single
     * recv can straddle the CS0/CS1 boundary — must split or CS1 never starts
     * and the panel only shows one half.
     *
     * Always copy through s_chunk (MALLOC_CAP_DMA). lastfrm/stream buffers are
     * often non-DMA; after Wi-Fi/NimBLE, spi_master cannot alloc priv TX and
     * the panel gets a dirty/incomplete frame (no Display Done). */
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
        if (n > CHUNK_SIZE) {
            n = CHUNK_SIZE;
        }
        memcpy(s_chunk, dmaChunk + off, (size_t)n);
        writeEpdData(s_chunk, (unsigned int)n);
        if (last_frame_capturing()) {
            last_frame_capture_write(s_chunk, (size_t)n);
        }
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
    int rc = 0;
    if (s_uploadCount != EPD_HALF_BYTES * 2) {
        ESP_LOGW(TAG, "upload size mismatch: %lu", (unsigned long)s_uploadCount);
        last_frame_capture_abort();
        s_panelDirty = true;
        rc = -1;
    } else {
        /* Refresh while still holding the panel lock so nothing else can
         * interleave DTM/PON with this frame (avoids dirty corners). */
        if (epd_commit_display()) {
            last_frame_capture_commit();
        } else {
            last_frame_capture_abort();
            rc = -1;
        }
    }
    /* Drop jobs queued during the long upload (MQTT render_text, etc.). */
    render_queue_drop_pending("upload finish");
    epd_unlock();
    if (rc == 0) {
        app_epd_note_user_content();
    }
    return rc;
}

void epd_upload_abort(void)
{
    if (s_uploadCsx >= 0) {
        stream_end(s_uploadCsx);
        s_uploadCsx = -1;
    }
    s_uploadCount = 0;
    s_panelDirty = true;
    last_frame_capture_abort();
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
        /* display_url + SoftAP proxyImage share this TLS stack. A second
         * large task starves httpd on C61 internal RAM (ESP_ERR_HTTPD_TASK). */
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
    s_panelDirty = true; /* power cycle — first paint is a full DRF of content */
    s_panelOk = (checkDriverICStatus() == DONE);
    if (!s_panelOk) {
        vTaskDelay(pdMS_TO_TICKS(50));
        s_panelOk = (checkDriverICStatus() == DONE);
    }
    if (!s_panelOk) {
        /* Keep streaming attempts (BUSY rose + SPI init), but report unhealthy. */
        ESP_LOGW(TAG, "driver IC status not ready — draws allowed, panel_ok=false");
    } else {
        ESP_LOGI(TAG, "panel ready");
    }
    return 0;
}

bool app_epd_jobs_accepted(void)
{
    return s_acceptJobs;
}

bool app_epd_wait_idle(int timeout_ms)
{
    if (!s_panelLock) {
        return true;
    }
    TickType_t ticks = (timeout_ms < 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    if (xSemaphoreTake(s_panelLock, ticks) != pdTRUE) {
        ESP_LOGW(TAG, "wait idle timeout (%d ms) — panel still busy", timeout_ms);
        return false;
    }
    xSemaphoreGive(s_panelLock);
    return true;
}

bool app_epd_sleep_prepare(int timeout_ms)
{
    if (!s_panelLock) {
        return true;
    }
    /* 1) Stop new work immediately so nothing queues during the wait. */
    s_acceptJobs = false;
    render_queue_drop_pending("sleep prepare");

    /* 2) Wait for in-flight render/upload to release the panel, then HOLD it
     * so mqtt_stop / wifi_stop / EPAD cut cannot race a new refresh. */
    TickType_t ticks = (timeout_ms < 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    if (xSemaphoreTake(s_panelLock, ticks) != pdTRUE) {
        ESP_LOGW(TAG, "sleep prepare timeout (%d ms) — resume jobs", timeout_ms);
        s_acceptJobs = true;
        s_sleepHoldsLock = false;
        return false;
    }
    /* Drop anything that slipped in before the gate closed. */
    render_queue_drop_pending("sleep prepare held");
    s_sleepHoldsLock = true;
    ESP_LOGI(TAG, "sleep prepare OK — panel lock held, jobs paused");
    return true;
}

void app_epd_sleep_abort(void)
{
    s_acceptJobs = true;
    if (s_sleepHoldsLock && s_panelLock) {
        s_sleepHoldsLock = false;
        xSemaphoreGive(s_panelLock);
        ESP_LOGI(TAG, "sleep abort — panel lock released, jobs resumed");
    } else {
        s_sleepHoldsLock = false;
    }
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
    bool do_cap = s_stream_cap_src[0] != '\0';
    if (do_cap) {
        if (last_frame_capture_begin(s_stream_cap_src) != 0) {
            do_cap = false;
        }
        s_stream_cap_src[0] = '\0';
    }
    memset(s_chunk, color_code, CHUNK_SIZE);
    for (int csx = 0; csx < 2; csx++) {
        stream_begin(csx);
        for (unsigned long sent = 0; sent < EPD_HALF_BYTES; sent += CHUNK_SIZE) {
            unsigned long n = EPD_HALF_BYTES - sent;
            if (n > CHUNK_SIZE) {
                n = CHUNK_SIZE;
            }
            writeEpdData(s_chunk, (unsigned int)n);
            if (do_cap) {
                last_frame_capture_write(s_chunk, (size_t)n);
            }
        }
        stream_end(csx);
    }
    if (epd_commit_display()) {
        if (do_cap) {
            last_frame_capture_commit();
        }
    } else if (do_cap) {
        last_frame_capture_abort();
    }
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
        s_lastDisplayOk = false;
        return;
    }
    if (canvas_fill_text_screen(cv, lines, line_count, fg_code, bg_code, scale) != 0) {
        ESP_LOGE(TAG, "text screen fill failed");
        s_lastDisplayOk = false;
        free(cv);
        return;
    }
    canvas_render(cv);
    free(cv);
}

void epd_text_paragraph(const char *text, unsigned char fg_code,
                        unsigned char bg_code, int scale_req)
{
    if (!text || !text[0]) {
        return;
    }
    char (*lines)[CANVAS_TEXT_MAX] = calloc((size_t)EPD_TEXT_MAX_LINES, CANVAS_TEXT_MAX);
    if (!lines) {
        ESP_LOGE(TAG, "paragraph wrap OOM");
        s_lastDisplayOk = false;
        return;
    }
    int scale = scale_req;
    bool trunc = false;
    int n = canvas_wrap_text_fit(text, scale_req, lines, EPD_TEXT_MAX_LINES,
                                 &scale, &trunc);
    ESP_LOGI(TAG, "paragraph %u bytes -> %d lines scale %d→%d%s",
             (unsigned)strlen(text), n, scale_req, scale, trunc ? " (truncated)" : "");
    const char *ptrs[EPD_TEXT_MAX_LINES] = { 0 };
    for (int i = 0; i < n && i < EPD_TEXT_MAX_LINES; i++) {
        ptrs[i] = lines[i];
    }
    epd_text_screen(ptrs, n, fg_code, bg_code, scale);
    free(lines);
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

typedef struct {
    const uint8_t *codes;
    int sw, sh;
} codes_cover_ctx_t;

static unsigned char codes_cover_pixel(int lx, int ly, void *ctx)
{
    const codes_cover_ctx_t *c = (const codes_cover_ctx_t *)ctx;
    return img_codes_cover_pixel(c->codes, c->sw, c->sh, lx, ly);
}

static int stream_raw_native(const uint8_t *raw, size_t len)
{
    if (len != (size_t)EPD_HALF_BYTES * 2) {
        return -1;
    }
    const bool cap = last_frame_capturing();
    size_t off = 0;
    for (int csx = 0; csx < 2; csx++) {
        stream_begin(csx);
        for (unsigned long sent = 0; sent < EPD_HALF_BYTES;) {
            int n = (int)((EPD_HALF_BYTES - sent) > CHUNK_SIZE ? CHUNK_SIZE
                                                               : (EPD_HALF_BYTES - sent));
            memcpy(s_chunk, raw + off, (size_t)n);
            writeEpdData(s_chunk, n);
            if (cap) {
                last_frame_capture_write(s_chunk, (size_t)n);
            }
            off += (size_t)n;
            sent += n;
        }
        stream_end(csx);
    }
    return epd_commit_display() ? 0 : -1;
}

/* Progress is UART/evtlog only — a full E6 refresh per stage costs ~40s and
 * used to make previously-working URLs fail under the job deadline. */
static void url_stage(int show, const char *stage)
{
    if (!stage) {
        return;
    }
    ESP_LOGI(TAG, "url stage%s: %s", show ? "" : " (quiet)", stage);
}

static void url_heap_log(const char *stage)
{
    ESP_LOGI(TAG,
             "url mem[%s] free=%u spiram=%u largest_s=%u int=%u largest_i=%u",
             stage ? stage : "?",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

int epd_url_frame(const char *url)
{
    s_url_err[0] = '\0';
    if (!s_acceptJobs) {
        url_fail("sleep pending");
        return -1;
    }
    ESP_LOGI(TAG, "url frame start: %.96s%s", url ? url : "",
             (url && strlen(url) > 96) ? "…" : "");
    evtlog_event("EPD", "display_url start %.80s", url ? url : "");
    url_heap_log("start");

    powerAttr_t pwr;
    cfg_get_power_attr(&pwr);
    const int quality = (pwr.imageQuality != IMG_QUALITY_SPEED);
    const img_proc_mode_t proc = quality ? IMG_PROC_QUALITY : IMG_PROC_SPEED;
    ESP_LOGI(TAG, "url image mode=%s", img_quality_str(pwr.imageQuality));

    /* Do not stage before download: raw path needs no progress page, and each
     * stage is a full-panel refresh (tens of seconds). JPEG/PNG stages below. */

    const size_t raw_need = (size_t)EPD_HALF_BYTES * 2;
    const int64_t t0 = esp_timer_get_time();
    const int64_t net_deadline = t0 + (int64_t)IMG_URL_NET_DEADLINE_MS * 1000;
    const int64_t decode_deadline = t0 + (int64_t)IMG_URL_DECODE_DEADLINE_MS * 1000;

    url_tls_prepare();
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = IMG_URL_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .max_redirection_count = 5,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "url http init fail — free %u int %u largest_int %u",
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        url_fail("http init fail");
        url_tls_release();
        return -1;
    }

    uint8_t *body = NULL;
    uint8_t *fb = NULL;
    int ret = -1;

    if (esp_http_client_open(client, 0) != ESP_OK) {
        url_fail("http open/TLS fail");
        goto out;
    }
    if (esp_timer_get_time() > net_deadline) {
        url_fail("URL net timeout");
        goto out_close;
    }
    long clen = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "url headers status=%d clen=%ld (%.1fs)",
             status, clen,
             (esp_timer_get_time() - t0) / 1e6);
    if (status > 0 && (status < 200 || status >= 300)) {
        char buf[40];
        snprintf(buf, sizeof(buf), "HTTP status %d", status);
        url_fail(buf);
        goto out_close;
    }

    /* Reject oversized BEFORE allocating / buffering — keeps device alive. */
    size_t cap = IMG_URL_MAX_BYTES;
    if (clen > 0) {
        if ((size_t)clen > IMG_URL_MAX_BYTES && (size_t)clen != raw_need) {
            char buf[40];
            snprintf(buf, sizeof(buf), "file >%uKB limit",
                     (unsigned)(IMG_URL_MAX_BYTES / 1024));
            url_fail(buf);
            goto out_close;
        }
        if ((size_t)clen == raw_need) {
            cap = raw_need;
        } else {
            cap = (size_t)clen;
        }
    }

    /* Large bodies must stay in PSRAM — internal malloc fallback steals TLS/Wi‑Fi
     * headroom and looks like a random hang/OOM later. */
    body = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!body) {
        url_heap_log("body_oom");
        url_fail("download OOM");
        goto out_close;
    }
    url_heap_log("body_alloc");

    size_t total = 0;
    size_t last_log = 0;
    while (total < cap) {
        if (esp_timer_get_time() > net_deadline) {
            url_fail("URL net timeout");
            goto out_close;
        }
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
        if (total - last_log >= (128 * 1024) || total == cap) {
            ESP_LOGI(TAG, "url download %u/%u (%.1fs)",
                     (unsigned)total, (unsigned)cap,
                     (esp_timer_get_time() - t0) / 1e6);
            last_log = total;
        }
    }
    /* Chunked / missing Content-Length: filled the cap and still more data → reject. */
    if (total == cap && (size_t)clen != raw_need) {
        char probe[1];
        int extra = esp_http_client_read(client, probe, 1);
        if (extra > 0) {
            char buf[40];
            snprintf(buf, sizeof(buf), "file >%uKB limit",
                     (unsigned)(IMG_URL_MAX_BYTES / 1024));
            url_fail(buf);
            goto out_close;
        }
    }
    ESP_LOGI(TAG, "downloaded %u bytes (clen=%ld)", (unsigned)total, clen);
    url_heap_log("downloaded");
    if (total == 0) {
        url_fail("empty download");
        goto out_close;
    }
    if (!s_acceptJobs) {
        url_fail("sleep pending");
        goto out_close;
    }
    if (esp_timer_get_time() > decode_deadline) {
        url_fail("URL decode timeout");
        goto out_close;
    }

    /* Drop TLS/HTTP buffers before decode+dither — they eat hundreds of KB
     * of PSRAM and were the main cause of dither OOM on 2MB C61. */
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    client = NULL;
    url_heap_log("tls_freed");

    img_fmt_t fmt = img_sniff_format(body, total);
    if (fmt == IMG_FMT_RAW4BPP || (total == raw_need && fmt != IMG_FMT_JPEG && fmt != IMG_FMT_PNG)) {
        if (!s_acceptJobs) {
            url_fail("sleep pending");
            goto out;
        }
        epd_lock();
        last_frame_capture_begin("url");
        ret = stream_raw_native(body, total);
        if (ret == 0) {
            last_frame_capture_commit();
            ESP_LOGI(TAG, "raw 4bpp frame OK");
        } else {
            last_frame_capture_abort();
            url_fail("raw stream fail");
        }
        epd_unlock();
    } else if (fmt == IMG_FMT_JPEG || fmt == IMG_FMT_PNG) {
        if (esp_timer_get_time() > decode_deadline) {
            url_fail("URL decode timeout");
            goto out;
        }
        /* Prefer SPEED automatically when body is large — QUALITY decode+dither
         * can starve RAM and look like a hang on 2MB PSRAM. */
        img_proc_mode_t use_proc = proc;
        if (total > IMG_URL_FORCE_SPEED_BYTES && use_proc == IMG_PROC_QUALITY) {
            ESP_LOGW(TAG, "large body %u — force SPEED decode", (unsigned)total);
            use_proc = IMG_PROC_SPEED;
        }
        url_stage(use_proc == IMG_PROC_QUALITY, "Decoding...");
        url_heap_log("pre_decode");
        uint8_t *rgb = NULL;
        int rw = 0, rh = 0;
        if (img_decode_to_rgb(body, total, &rgb, &rw, &rh, use_proc) != 0) {
            url_fail(fmt == IMG_FMT_JPEG ? "JPEG decode fail" : "PNG decode fail");
        } else {
            free(body);
            body = NULL;
            url_heap_log("post_decode");
            if (esp_timer_get_time() > decode_deadline) {
                free(rgb);
                url_fail("URL decode timeout");
                goto out;
            }
            if (!s_acceptJobs) {
                free(rgb);
                url_fail("sleep pending");
                goto out;
            }
            url_stage(use_proc == IMG_PROC_QUALITY, "Dithering...");
            /* Prefer codes→stream cover: avoids a contiguous 960KB FB alloc that
             * often fails after TLS download fragmentation on 2MB PSRAM. */
            uint8_t *codes = NULL;
            int cw = 0, ch = 0;
            if (img_rgb_dither_codes(rgb, rw, rh, &codes, &cw, &ch, use_proc) != 0) {
                free(rgb);
                url_fail("dither OOM/fail");
            } else {
                free(rgb); /* dither owns the output codes; drop RGB early */
                codes_cover_ctx_t cctx = {.codes = codes, .sw = cw, .sh = ch};
                url_heap_log("pre_stream");
                ESP_LOGI(TAG, "decode OK — stream cover %dx%d", cw, ch);
                if (!s_acceptJobs) {
                    free(codes);
                    url_fail("sleep pending");
                    goto out;
                }
                epd_lock();
                last_frame_capture_begin("url");
                if (stream_render_unlocked(codes_cover_pixel, &cctx)) {
                    last_frame_capture_commit();
                    ret = 0;
                    url_heap_log("done");
                    ESP_LOGI(TAG, "decoded image frame OK (%s)",
                             fmt == IMG_FMT_JPEG ? "JPEG" : "PNG");
                } else {
                    last_frame_capture_abort();
                    url_fail("display incomplete");
                }
                epd_unlock();
                free(codes);
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
    url_tls_release();
    return ret;
}
