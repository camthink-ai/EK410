/**
 * @file img_dither.h
 * @brief Dither a decoded RGB image straight onto the panel's native 4bpp frame.
 *
 * Pure C — no ESP-IDF — so tools/tests runs the very same code on the host.
 */
#ifndef IMG_DITHER_H_
#define IMG_DITHER_H_

#include <stddef.h>
#include <stdint.h>

/* Logical landscape size of the panel the frame is written for. */
#define IMG_DITHER_PANEL_W 1600
#define IMG_DITHER_PANEL_H 1200

/**
 * Cover-fit `rgb` (w×h, 3 bytes per pixel) onto the panel and dither it into
 * `frame` — the native 960000-byte layout: two IC halves of 1600 lines ×
 * 300 bytes (logical x = native line, logical y = 1199 − native x), CS0 half
 * first. The image is upsampled bilinearly on the fly, so nothing the size of
 * the panel is allocated.
 *
 * grid_w × grid_h is the dither grid: 1600×1200 gives one ink dot per panel
 * pixel, 800×600 gives 2×2 dots. Both must divide the panel size.
 *
 * `alloc` provides the working memory (≈ 25 KB + 9 bytes per grid column and
 * 8 bytes per grid row) and `kick` is called every 16 dither columns so the
 * caller can feed a watchdog. Returns 0, or -1 on bad arguments or when
 * `alloc` fails — the frame is then left untouched.
 */
int img_dither_native(const uint8_t *rgb, int w, int h, int grid_w, int grid_h,
                      uint8_t *frame, void *(*alloc)(size_t n), void (*kick)(void));

/* The same with the EXIF orientation of the picture (1–8) applied where the
 * source is sampled: 1 leaves the frame byte-identical to img_dither_native. */
int img_dither_native_o(const uint8_t *rgb, int w, int h, int orient, int grid_w, int grid_h,
                        uint8_t *frame, void *(*alloc)(size_t n), void (*kick)(void));

#endif /* IMG_DITHER_H_ */
