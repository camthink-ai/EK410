/**
 * @file img_decode.h
 * @brief Decode JPEG/PNG into a logical 1600×1200 4bpp Spectra framebuffer.
 *
 * JPEG uses ROM TJpgDec (MCU output). PNG uses libpng row-by-row.
 * WebP is not supported on-device (2MB PSRAM) — platform must convert.
 */
#ifndef IMG_DECODE_H_
#define IMG_DECODE_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IMG_PANEL_W 1600
#define IMG_PANEL_H 1200
/* Packed 4bpp: 2 pixels/byte → 800 bytes/row × 1200 rows */
#define IMG_FB_BYTES ((IMG_PANEL_W / 2) * IMG_PANEL_H)

typedef enum {
    IMG_FMT_UNKNOWN = 0,
    IMG_FMT_RAW4BPP,
    IMG_FMT_JPEG,
    IMG_FMT_PNG,
    IMG_FMT_WEBP, /* detected but not decoded on-device */
} img_fmt_t;

/* Matches config img_quality_t; kept local so img_decode stays free of config.h. */
typedef enum {
    IMG_PROC_SPEED = 0,
    IMG_PROC_QUALITY = 1,
} img_proc_mode_t;

img_fmt_t img_sniff_format(const uint8_t *data, size_t len);

/**
 * Decode JPEG/PNG to heap RGB888 (*rgb_out, w×h). Caller frees *rgb_out.
 * Prefer calling this before allocating the 960KB panel FB so more RAM
 * stays available for a higher-res dither working set.
 * mode=QUALITY uses a larger RGB budget (slower, less grain after cover).
 */
int img_decode_to_rgb(const uint8_t *data, size_t len, uint8_t **rgb_out, int *w_out,
                      int *h_out, img_proc_mode_t mode);

/**
 * Same as img_decode_to_rgb, but the file may live in up to `nparts` pieces that
 * concatenate in order to `len` bytes. JPEG/PNG decoders already read through
 * callbacks, so they never need one contiguous block.
 */
#define IMG_DECODE_MAX_PARTS 3
int img_decode_to_rgb_parts(const uint8_t *const *parts, const size_t *part_lens, int nparts,
                            size_t len, uint8_t **rgb_out, int *w_out, int *h_out,
                            img_proc_mode_t mode);

/**
 * SoftAP-style E6 FS at source res + cover into panel. Consumes/frees `rgb`.
 * Returns a process-lifetime panel FB in *fb_out (do not free — use
 * img_fb_release). QUALITY may nearest-upscale RGB toward the budget before FS.
 * On 2MB C61 this may fail after a large HTTPS body (fragmentation); prefer
 * img_rgb_dither_codes + streaming cover for the URL path.
 */
int img_rgb_dither_alloc_fb(uint8_t *rgb, int w, int h, uint8_t **fb_out,
                            img_proc_mode_t mode);

/**
 * Dither RGB → packed ink codes (*codes_out, length w*h). Consumes/frees `rgb`.
 * Caller frees *codes_out. Use with img_codes_cover_pixel + panel stream.
 */
int img_rgb_dither_codes(uint8_t *rgb, int w, int h, uint8_t **codes_out, int *cw_out,
                         int *ch_out, img_proc_mode_t mode);

/**
 * URL path, preferred: smooth cover-fit and a dither at (nearly) panel
 * resolution — QUALITY one ink dot per panel pixel, SPEED 800×600 — written
 * straight into `frame`, the native 960000-byte layout the panel streams.
 * No panel framebuffer, no cover sampling afterwards. Needs ≈ 90 KB of
 * working memory. Frees `rgb` and returns 0; on failure returns -1 and
 * leaves `rgb` to the caller (fall back to img_rgb_dither_codes).
 */
int img_render_native(uint8_t *rgb, int w, int h, img_proc_mode_t mode, uint8_t *frame);

/** Cover-fit sample: logical (lx,ly) on 1600×1200 → ink from codes[sw×sh]. */
uint8_t img_codes_cover_pixel(const uint8_t *codes, int sw, int sh, int lx, int ly);

/** Convenience: decode + dither; *fb_out via img_rgb_dither_alloc_fb. */
int img_decode_alloc_fb(const uint8_t *data, size_t len, uint8_t **fb_out,
                        img_proc_mode_t mode);

/** Release FB from img_*_alloc_fb (no-op for the singleton panel buffer). */
void img_fb_release(uint8_t *fb);

#ifdef __cplusplus
}
#endif

#endif /* IMG_DECODE_H_ */
