/**
 * @file last_frame.h
 * @brief Persist the last successful native panel frame (960000B) in flash.
 *
 * Used so SoftAP / LAN clients can preview what is currently on the e-ink
 * panel (uploadFrame, display_url, canvas, etc.).
 */
#ifndef LAST_FRAME_H_
#define LAST_FRAME_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LAST_FRAME_BYTES 960000
#define LAST_FRAME_SOURCE_MAX 24

typedef struct {
    bool present;
    uint32_t seq;
    uint32_t crc32;
    int64_t saved_us; /* esp_timer_get_time() when saved */
    char source[LAST_FRAME_SOURCE_MAX];
} last_frame_meta_t;

void last_frame_init(void);

/** True between begin and commit/abort. */
bool last_frame_capturing(void);

/** Begin capturing a native frame into a SPIRAM scratch buffer. */
int last_frame_capture_begin(const char *source);

/** Append native frame bytes (same layout as uploadFrame body). */
int last_frame_capture_write(const uint8_t *data, size_t len);

/** Erase+program flash from the capture buffer after a successful refresh. */
int last_frame_capture_commit(void);

void last_frame_capture_abort(void);

/**
 * The capture buffer itself — 960000 bytes, kept for the process lifetime —
 * for a renderer that builds a native frame in it (img_dither_native()). NULL
 * when last-frame is disabled or the buffer was never allocated. Hold the
 * panel lock, then hand the finished frame to last_frame_commit_native().
 */
uint8_t *last_frame_buffer(void);

/** Save what is in the capture buffer as the last frame: flash + telemetry preview. */
int last_frame_commit_native(const char *source);

bool last_frame_capture_active(void);

/** Load meta from flash (cheap). */
int last_frame_get_meta(last_frame_meta_t *out);

/**
 * Cached MQTT preview of the last frame (≤200KB 8bpp BMP, base64).
 * Prefer last_frame_preview_snapshot() from MQTT — raw getters are not
 * safe across concurrent preview_drop / rebuild.
 */
const char *last_frame_preview_b64(void);
int last_frame_preview_w(void);
int last_frame_preview_h(void);
/** Always "image/bmp" when preview is present. */
const char *last_frame_preview_mime(void);

typedef struct {
    char *b64; /* heap copy — caller free(); NULL if no preview */
    int w;
    int h;
} last_frame_preview_snap_t;

/**
 * Copy preview b64 + dimensions under the preview lock (same frame).
 * Returns 0 always; out->b64 NULL when unavailable. Caller frees out->b64.
 */
int last_frame_preview_snapshot(last_frame_preview_snap_t *out);

/**
 * Free in-RAM preview (≈160KB SPIRAM). Call before large URL decode;
 * rebuilds automatically on the next successful frame commit.
 */
void last_frame_preview_drop(void);

/**
 * Build the preview again from the saved frame when a fetch dropped it and
 * nothing was committed (failed display, unchanged frame, proxyImage). No-op
 * while a preview exists. Reads the frame buffer: hold the panel lock
 * (app_epd_preview_restore() does).
 */
void last_frame_preview_restore(void);

/**
 * Read the native frame into caller buffer (must be LAST_FRAME_BYTES).
 * Returns 0 on success, -1 if missing/corrupt.
 */
int last_frame_read(uint8_t *out, size_t out_len);

/**
 * Stream the native frame in small flash reads (no 960KB alloc).
 * write_fn returns 0 to continue, nonzero to abort.
 * Returns 0 on success, -1 missing/corrupt/IO, -2 another stream in progress.
 */
typedef int (*last_frame_write_fn)(const uint8_t *data, size_t len, void *ctx);
int last_frame_stream(last_frame_write_fn write_fn, void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* LAST_FRAME_H_ */
