#ifndef APP_EPD_H_
#define APP_EPD_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Full panel power-up + init (vendor demo main() sequence). */
int app_epd_init(void);

/* Fill the whole panel with one color code (e.g. WHITE 0x11) and refresh. */
void epd_fill_refresh(unsigned char color_code);

/* scale=3 ≈ 21 lines fit; keep headroom for scale=2 long MQTT text. */
#define EPD_TEXT_MAX_LINES 24
/* Raw MQTT/HTTP paragraph before wrap (keep under CMD_BUF). */
#define EPD_TEXT_RAW_MAX   2048

/* Render up to EPD_TEXT_MAX_LINES of UTF-8 text (ASCII + GB2312),
 * then refresh. Logical landscape 1600x1200. scale >= 1. */
void epd_text_screen(const char *const *lines, int line_count,
                     unsigned char fg_code, unsigned char bg_code, int scale);
/* Wrap a raw paragraph on the render task, then paint (top-left layout). */
void epd_text_paragraph(const char *text, unsigned char fg_code,
                        unsigned char bg_code, int scale_req);

/* HTTP-GET a frame URL and refresh. Accepts:
 *  - raw 960000-byte native 4bpp
 *  - JPEG / PNG (on-device decode → Spectra 6-ink, letterboxed 1600×1200)
 * WebP is not decoded on-device (2MB PSRAM) — convert on the platform. */
int epd_url_frame(const char *url);

/* Last epd_url_frame failure reason (English, short); "" if last call OK. */
const char *epd_url_last_error(void);

/* Boot/status text (logical landscape). */
void epd_boot_screen(const char *title, const char *const *lines, int line_count);

/* Default splash (landscape CamThink logo, no caption). */
void epd_logo_screen(void);

/* Queue default splash on the render task. */
int app_epd_splash_async(void);

/* Queue replay of flash lastfrm; -1 if none stored. */
int app_epd_last_frame_async(void);

/**
 * Sync boot paint BEFORE Wi-Fi init (DMA RAM still free).
 * lastfrm if present, else CamThink splash.
 */
int app_epd_boot_paint_sync(void);

/* ---- async render API (for HTTP/MQTT callers; a full refresh blocks
 * the display task for 20-40 s and must not stall a request handler) ---- */

typedef struct {
    /* Raw paragraph for render_text (preferred). Empty → use lines[]. */
    char text[EPD_TEXT_RAW_MAX];
    /* Pre-split lines (boot/setup). UTF-8; ~60 CJK / ~190 ASCII each. */
    char lines[EPD_TEXT_MAX_LINES][192];
    int count;
    unsigned char fg, bg;
    int scale; /* requested; paragraph path may shrink to fit */
} espd_render_t;

/* Queue a text render; returns 0 if queued, -1 if a render is in flight. */
int app_epd_render_async(const espd_render_t *render);

/* Queue the boot test pattern; returns 0 if queued, -1 if busy. */
int app_epd_test_pattern_async(void);

/* Queue a bare refresh (after raw data was streamed by the upload handler). */
int app_epd_refresh_async(void);

/* Panel answered its status probe at init (drives the fault LED). */
bool app_epd_panel_ok(void);

/* Wait until no render/upload holds the panel lock. Used before deep sleep
 * cuts EPAD — cutting mid-DRF leaves striped/incomplete gaps on the glass. */
bool app_epd_wait_idle(int timeout_ms);

/**
 * Prepare for deep sleep: stop accepting new render/upload jobs, drop the
 * queue, then wait for any in-flight panel work and HOLD the panel lock.
 * On timeout, resumes accepting and returns false. On success the caller must
 * either deep-sleep (never returns) or call app_epd_sleep_abort().
 */
bool app_epd_sleep_prepare(int timeout_ms);

/** Undo app_epd_sleep_prepare() after a deferred/cancelled sleep. */
void app_epd_sleep_abort(void);

/** False while sleep_prepare holds the gate (callers should return busy). */
bool app_epd_jobs_accepted(void);

/** True while a render job is running or queued (Web polls after "queued"). */
bool app_epd_display_busy(void);
/** Result of the last panel commit / URL display (true = OK). */
bool app_epd_last_display_ok(void);

/* Queue the title+lines boot/setup screen. */
int app_epd_boot_screen_async(const char *title, const char *const *lines, int count);

/* SoftAP provisioning screen: large instructions + URL QR (scan after joining AP). */
int app_epd_setup_screen_async(const char *ssid);

/* Panel content provenance: SoftAP close restores logo only when guide was
 * shown and the user has not pushed new art since. */
void app_epd_note_user_content(void);
void app_epd_note_system_frame(void);
bool app_epd_has_user_content(void);

/* Queue the embedded flash test frame (frame_data.h). */
int app_epd_test_frame_async(void);

/* Queue a full-panel color fill. */
int app_epd_fill_async(unsigned char color_code);

/* Queue a raw-frame fetch+render from an HTTP URL (max 511 chars).
 * Returns 0 queued, -1 queue full, -2 sleep pending, -3 other. */
int app_epd_url_async(const char *url);

/** Map app_epd_*_async return code to a short error token for HTTP/MQTT. */
static inline const char *app_epd_queue_err(int rc)
{
    if (rc == 0) {
        return "queued";
    }
    if (rc == -2) {
        return "sleep pending";
    }
    if (rc == -1) {
        return "queue full";
    }
    return "render rejected";
}

/* Next epd_stream_render / fill will persist the native frame to flash. */
void app_epd_mark_frame_capture(const char *source);

/* SoftAP preview: download image bytes on epd_render (TLS stack), no panel.
 * On success: job->data/len/ctype set; caller frees job->data. Returns 0 / -1. */
typedef struct {
    char url[512];
    uint8_t *data;
    size_t len;
    char ctype[48];
    char err[48];
    volatile bool orphaned; /* HTTP timed out — render task cleans up */
} epd_proxy_job_t;

int app_epd_proxy_fetch(epd_proxy_job_t *job, int timeout_ms);

/* Raw frame upload streaming (used by the HTTP upload endpoint). The DMA
 * scratch buffer is shared — recv into it, then hand each chunk over. */
unsigned char *app_epd_dma_buf(void);
int app_epd_dma_buf_size(void);
int epd_upload_start(void);
int epd_upload_write(const unsigned char *dmaChunk, int len); /* halves auto-switch */
int epd_upload_finish(void); /* sync refresh + unlock */
void epd_upload_abort(void); /* unlock after failed/interrupted upload */

/* Low-level: full-panel render calling pixel_fn for every pixel (top row
 * first), streamed line by line; the callback's low nibble is used. */
void epd_stream_render(unsigned char (*pixel_fn)(int x, int y, void *ctx), void *ctx);

#endif
