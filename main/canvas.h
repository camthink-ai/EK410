/**
 * @file canvas.h
 * @brief Draw-list canvas: text / rects / QR codes at arbitrary positions
 */
#ifndef __CANVAS_H_
#define __CANVAS_H_

#include <stdbool.h>
#include <stddef.h>
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CANVAS_MAX_ITEMS 48
#define CANVAS_TEXT_MAX 192 /* UTF-8; ~60 CJK or 190 ASCII */
#define CANVAS_QR_SLOTS 2

/* QR encoder state is embedded in the canvas struct so a queued render can
 * not be corrupted by the next parse (version capped at 10: ~200 char URLs). */
#include "qrcodegen.h"
#define CANVAS_QR_BUF_LEN qrcodegen_BUFFER_LEN_FOR_VERSION(10)
#define CANVAS_QR_VERSION_MAX 10

typedef enum {
    CI_TEXT = 0,
    CI_RECT,
    CI_QR,
} canvas_item_type_t;

typedef struct {
    canvas_item_type_t type;
    int x, y;                 /* top-left, logical landscape (1600x1200)    */
    unsigned char color;      /* 4bpp code                                   */
    /* text */
    char text[CANVAS_TEXT_MAX];
    int scale;
    /* rect */
    int w, h, thickness;
    bool fill;
    /* qr */
    int qrSlot;               /* index into the encoded-payload slots        */
    int qrSize;               /* module count (from encoder)                 */
} canvas_item_t;

typedef struct {
    unsigned char bg;
    canvas_item_t items[CANVAS_MAX_ITEMS];
    int count;
    uint8_t qr[CANVAS_QR_SLOTS][CANVAS_QR_BUF_LEN]; /* encoded module bitmaps */
} canvas_t;

/* Map a color name ("black".."green") to the 4bpp panel code. */
unsigned char canvas_color(const char *name);

/* Parse {"bg":"white","items":[...]} into canvas_t. QR payloads are encoded
 * into shared slots immediately. Returns 0 on success. */
int canvas_parse_json(const cJSON *root, canvas_t *cv, char *errBuf, int errLen);

/* Async queue a parsed canvas for rendering; 0 = queued, -1 = busy. */
int app_epd_canvas_async(const canvas_t *cv);

/* SoftAP setup layout: large text (SSID/pass/URL) + QR for http://192.168.4.1.
 * Returns 0 on success. */
int canvas_fill_setup_screen(canvas_t *cv, const char *ssid);

/* Centered multiline UTF-8 text screen (ASCII 8x16 + GB2312 16x16).
 * Used by render_text so remote CJK works the same as SoftAP/canvas. */
int canvas_fill_text_screen(canvas_t *cv, const char *const *lines, int line_count,
                            unsigned char fg, unsigned char bg, int scale);

/* Copy UTF-8 without splitting a multibyte sequence. */
void canvas_utf8_strlcpy(char *dst, const char *src, size_t dstSz);

/* Render a canvas right now (blocking, 20-40 s) — used by the render task. */
void canvas_render(const canvas_t *cv);

#ifdef __cplusplus
}
#endif

#endif /* __CANVAS_H_ */
